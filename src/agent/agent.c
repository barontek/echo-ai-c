#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "agent.h"
#include "context.h"
#include "../tools/registry.h"
#include "../tools/tool.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"
#include "../session/session_manager.h"

Agent *agent_create(const AgentConfig *cfg)
{
    Agent *agent = calloc(1, sizeof(Agent));
    if (!agent) return NULL;

    agent->provider = get_provider(cfg->provider, cfg->model,
                                   cfg->base_url, cfg->num_ctx, cfg->keep_alive_secs);
    if (!agent->provider)
    {
        log_error("failed to create provider", "name", cfg->provider, NULL);
        free(agent);
        return NULL;
    }

    agent->model = str_dup(cfg->model);
    agent->system_prompt = str_dup(cfg->system_prompt ? cfg->system_prompt : "");
    agent->temperature = cfg->temperature;
    agent->timeout = cfg->timeout;
    agent->max_iterations = cfg->max_iterations;
    agent->max_context_messages = cfg->max_context_messages;
    agent->max_context_chars = cfg->max_context_chars;
    agent->parallel_tool_exec = cfg->parallel_tool_exec;
    agent->cancel_requested = 0;
    agent->cb = cb_create(5, 30000);

    if (!agent->model)
    {
        agent_destroy(agent);
        return NULL;
    }

    return agent;
}

void agent_destroy(Agent *agent)
{
    if (!agent) return;
    if (agent->provider) agent->provider->destroy(agent->provider);
    if (agent->messages)
    {
        for (int i = 0; i < agent->messages_count; i++)
        {
            free(agent->messages[i].role);
            free(agent->messages[i].content);
            free(agent->messages[i].id);
            free(agent->messages[i].tool_call_id);
            free(agent->messages[i].tool_name);
            free(agent->messages[i].error_category);
            free(agent->messages[i].thinking);
            if (agent->messages[i].tool_calls)
            {
                for (int j = 0; j < agent->messages[i].tool_calls_count; j++)
                    tool_call_free(&agent->messages[i].tool_calls[j]);
                free(agent->messages[i].tool_calls);
            }
        }
        free(agent->messages);
    }
    free(agent->model);
    free(agent->system_prompt);
    free(agent->session_id);
    free(agent->context_summary);
    cb_destroy(agent->cb);
    free(agent);
}

static int agent_append_message(Agent *agent, Message *msg)
{
    int idx = agent->messages_count;
    int new_count = idx + 1;
    Message *new_msgs = realloc(agent->messages, sizeof(Message) * new_count);
    if (!new_msgs) return -1;

    new_msgs[idx] = *msg;
    agent->messages = new_msgs;
    agent->messages_count = new_count;
    return idx;
}

static void agent_save_session(Agent *agent);
static void agent_generate_title(Agent *agent);
static void agent_perform_summarization(Agent *agent, int original_count);
static int count_dropped_messages(Agent *agent, int *original);

static double time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int execute_tool_calls(Agent *agent, ToolCall *calls, int count)
{
    double tool_buckets[] = {0.01, 0.05, 0.1, 0.5, 1, 5, 10, 30};

    for (int i = 0; i < count; i++)
    {
        double start = time_sec();
        char *args_str = calls[i].arguments ? calls[i].arguments : "{}";
        const char *tname = calls[i].name ? calls[i].name : "unknown";

        cb_manager_tool_start(agent->cb_mgr, NULL, tname, args_str);

        Tool *tool = registry_get(calls[i].name);
        if (!tool)
        {
            if (agent->metrics)
                metrics_counter_inc(agent->metrics, "echo_tool_errors_total",
                                    "Total tool execution errors by name");
            Message *err_msg = message_create("tool", "tool not found");
            err_msg->tool_call_id = str_dup(calls[i].id ? calls[i].id : "");
            err_msg->tool_name = str_dup(tname);
            err_msg->error_category = str_dup("tool_not_found");
            agent_append_message(agent, err_msg);
            cb_manager_tool_error(agent->cb_mgr, NULL, tname, "tool not found");
            continue;
        }

        if (agent->on_approval)
        {
            int ok = agent->on_approval(calls[i].name, args_str, agent->approval_userdata);
            if (!ok)
            {
                Message *err_msg = message_create("tool", "tool call denied");
                err_msg->tool_call_id = str_dup(calls[i].id ? calls[i].id : "");
                err_msg->tool_name = str_dup(tname);
                err_msg->error_category = str_dup("denied");
                agent_append_message(agent, err_msg);
                cb_manager_tool_error(agent->cb_mgr, NULL, tname, "denied");
                continue;
            }
        }

        ToolResult *result = tool->execute(tool, args_str);
        if (!result)
            result = tool_result_error("tool returned no result", "execution_error");

        double elapsed = time_sec() - start;

        if (agent->metrics)
        {
            metrics_histogram_observe(agent->metrics, "echo_tool_duration_seconds",
                                      "Tool execution duration in seconds",
                                      elapsed, tool_buckets, 8);
            if (result->error)
                metrics_counter_inc(agent->metrics, "echo_tool_errors_total",
                                    "Total tool execution errors by name");
        }

        Message *tool_msg = message_create("tool",
                                            result->content ? result->content : "");
        tool_msg->tool_call_id = str_dup(calls[i].id ? calls[i].id : "");
        tool_msg->tool_name = str_dup(calls[i].name);
        if (result->error)
        {
            tool_msg->error_category = str_dup(result->error_category ? result->error_category : "execution_error");
            char *err_content = NULL;
            if (asprintf(&err_content, "Error: %s", result->error) < 0)
                err_content = str_dup("Error");
            free(tool_msg->content);
            tool_msg->content = err_content;
        }

        agent_append_message(agent, tool_msg);
        if (result->error)
            cb_manager_tool_error(agent->cb_mgr, NULL, tname, result->error);
        else
            cb_manager_tool_end(agent->cb_mgr, NULL, tname, result->content);
        tool_result_free(result);
    }

    return 0;
}

static int build_system_prompt(Agent *agent, char **out, size_t *out_len)
{
    const char *base = agent->system_prompt ? agent->system_prompt : "";
    if (agent->context_summary)
    {
        if (asprintf(out, "%s\n\nPrevious conversation summary: %s",
                     base, agent->context_summary) < 0)
            return -1;
    }
    else
    {
        *out = str_dup(base);
    }
    if (out_len && *out) *out_len = strlen(*out);
    return 0;
}

static void inject_system_with_summary(Agent *agent)
{
    char *sys = NULL;
    if (build_system_prompt(agent, &sys, NULL) != 0) return;

    int found = 0;
    for (int i = 0; i < agent->messages_count; i++)
    {
        if (strcmp(agent->messages[i].role, "system") == 0)
        {
            free(agent->messages[i].content);
            agent->messages[i].content = sys;
            found = 1;
            break;
        }
    }

    if (!found)
    {
        Message sys_msg = {0};
        sys_msg.role = str_dup("system");
        sys_msg.content = sys;
        Message *new_msgs = realloc(agent->messages, sizeof(Message) * (agent->messages_count + 1));
        if (new_msgs)
        {
            memmove(new_msgs + 1, new_msgs, sizeof(Message) * agent->messages_count);
            new_msgs[0] = sys_msg;
            agent->messages = new_msgs;
            agent->messages_count++;
        }
        else
        {
            free(sys);
        }
    }
}

static int count_dropped_messages(Agent *agent, int *original)
{
    (void)agent;
    return *original - agent->messages_count;
}

static LLMResponse *agent_llm_call(Agent *agent)
{
    if (agent->cb && !cb_is_available(agent->cb))
    {
        if (agent->metrics)
            metrics_counter_inc(agent->metrics, "echo_llm_cb_rejected_total",
                                "LLM calls rejected by circuit breaker");
        log_error("circuit breaker open, skipping LLM call", NULL);
        return NULL;
    }

    inject_system_with_summary(agent);

    cb_manager_llm_start(agent->cb_mgr, NULL, agent->messages_count);

    int original_count = agent->messages_count;
    int current_messages = agent->messages_count;
    Message *ctx_msgs = apply_context_window(agent->messages, &current_messages,
                                             agent->max_context_messages,
                                             agent->max_context_chars);
    if (ctx_msgs != agent->messages)
    {
        int dropped = count_dropped_messages(agent, &original_count);
        if (dropped > 0) agent_perform_summarization(agent, dropped);

        for (int i = 0; i < agent->messages_count; i++)
        {
            free(agent->messages[i].role);
            free(agent->messages[i].content);
            free(agent->messages[i].id);
            free(agent->messages[i].tool_call_id);
            free(agent->messages[i].tool_name);
            free(agent->messages[i].error_category);
            free(agent->messages[i].thinking);
        }
        free(agent->messages);
        agent->messages = ctx_msgs;
        agent->messages_count = current_messages;
    }

    double start = time_sec();
    double llm_buckets[] = {0.5, 1, 2, 5, 10, 30, 60, 120};

    LLMResponse *resp = agent->provider->chat(
        agent->provider, agent->messages, agent->messages_count,
        agent->model, agent->temperature, agent->timeout);

    double elapsed = time_sec() - start;

    if (agent->metrics)
    {
        metrics_histogram_observe(agent->metrics, "echo_llm_duration_seconds",
                                  "LLM call duration in seconds",
                                  elapsed, llm_buckets, 8);
        metrics_counter_inc(agent->metrics, "echo_llm_calls_total",
                            "Total LLM calls");
        if (!resp)
            metrics_counter_inc(agent->metrics, "echo_llm_errors_total",
                                "Total LLM call errors");
    }

    if (resp && agent->cb) cb_record_success(agent->cb);
    else if (!resp && agent->cb) cb_record_failure(agent->cb);

    if (resp) cb_manager_llm_end(agent->cb_mgr, NULL, resp->content);
    else cb_manager_llm_end(agent->cb_mgr, NULL, NULL);

    return resp;
}

static int tool_calls_remaining(ToolCall *calls, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (calls[i].name && calls[i].name[0] != '\0')
            return 1;
    }
    return 0;
}

static void agent_generate_title(Agent *agent)
{
    if (!agent || !agent->provider) return;
    if (!agent->sm || !agent->session_id) return;

    Session *s = session_manager_load_session(agent->sm, agent->session_id);
    if (!s || s->title_generation_attempted) { if (s) session_free(s); return; }
    session_free(s);

    s = session_manager_load_session(agent->sm, agent->session_id);
    if (!s) return;
    s->title_generation_attempted = 1;
    session_manager_save_session(agent->sm, s);
    session_free(s);

    Message title_msgs[2];
    memset(title_msgs, 0, sizeof(title_msgs));
    title_msgs[0].role = str_dup("system");
    title_msgs[0].content = str_dup("You generate very short titles for conversations.");

    /* build a "role: content\n..." excerpt, capped so long conversations
     * don't blow up the prompt for a 5-word title */
    const size_t cap = 4000;
    char *text = malloc(cap + 1);
    if (!text)
    {
        free(title_msgs[0].role);
        free(title_msgs[0].content);
        return;
    }
    size_t used = 0;
    text[0] = '\0';
    for (int i = 0; i < agent->messages_count && used < cap; i++)
    {
        const char *role = agent->messages[i].role ? agent->messages[i].role : "unknown";
        const char *content = agent->messages[i].content ? agent->messages[i].content : "";
        int n = snprintf(text + used, cap + 1 - used, "%s: %s\n", role, content);
        if (n < 0) break;
        if ((size_t)n >= cap + 1 - used) { used = cap; break; }
        used += (size_t)n;
    }

    /* instruction lives in the user message with the conversation clearly
     * delimited; small models paraphrase a bare system instruction instead
     * of following it (produced titles like "Conversation Title Request") */
    title_msgs[1].role = str_dup("user");
    if (asprintf(&title_msgs[1].content,
                 "Generate a very short title (5 words or fewer) for the conversation below. "
                 "Return ONLY the title, no quotes or punctuation.\n\nConversation:\n%s",
                 text) < 0)
        title_msgs[1].content = NULL;
    free(text);
    if (!title_msgs[1].role || !title_msgs[1].content)
    {
        free(title_msgs[0].role);
        free(title_msgs[0].content);
        free(title_msgs[1].role);
        free(title_msgs[1].content);
        return;
    }

    LLMResponse *resp = agent->provider->chat(
        agent->provider, title_msgs, 2,
        agent->model, 0.3, 15);

    free(title_msgs[0].role);
    free(title_msgs[0].content);
    free(title_msgs[1].role);
    free(title_msgs[1].content);

    if (resp && resp->content)
    {
        char *title = str_trim(resp->content);
        if (title && title[0])
        {
            Session *s2 = session_manager_load_session(agent->sm, agent->session_id);
            if (s2)
            {
                free(s2->title);
                s2->title = str_dup(title);
                session_manager_save_session(agent->sm, s2);
                session_free(s2);

                if (agent->on_title_update)
                    agent->on_title_update(agent->session_id, title, agent->title_userdata);
            }
        }
        llm_response_free(resp);
    }
}

static void agent_perform_summarization(Agent *agent, int original_count)
{
    (void)original_count;
    if (!agent || !agent->provider) return;

    int text_len = 0;
    for (int i = 0; i < agent->messages_count; i++)
    {
        if (agent->messages[i].content)
            text_len += strlen(agent->messages[i].content);
    }

    if (text_len > agent->max_context_chars * 2) return;

    char *text = malloc(text_len + 1);
    if (!text) return;
    text[0] = '\0';
    for (int i = 0; i < agent->messages_count; i++)
    {
        if (agent->messages[i].content)
            strcat(text, agent->messages[i].content);
    }

    Message sum_msgs[2];
    memset(sum_msgs, 0, sizeof(sum_msgs));
    sum_msgs[0].role = str_dup("system");
    sum_msgs[0].content = str_dup("Summarize this conversation concisely in 2-3 sentences.");

    char *truncated = text;
    if (text_len > 4000) { truncated[4000] = '\0'; }
    sum_msgs[1].role = str_dup("user");
    sum_msgs[1].content = str_dup(truncated);

    LLMResponse *resp = agent->provider->chat(
        agent->provider, sum_msgs, 2,
        agent->model, 0.3, 30);

    free(sum_msgs[0].role);
    free(sum_msgs[0].content);
    free(sum_msgs[1].role);
    free(sum_msgs[1].content);
    free(text);

    if (resp && resp->content)
    {
        free(agent->context_summary);
        agent->context_summary = str_trim(str_dup(resp->content));
        llm_response_free(resp);
    }
}

static unsigned long run_counter = 0;

static char *gen_run_id(void)
{
    char *id = NULL;
    run_counter++;
    if (asprintf(&id, "run_%lu", run_counter) < 0) return NULL;
    return id;
}

LLMResponse *agent_run(Agent *agent, const char *user_input)
{
    agent->cancel_requested = 0;

    char *run_id = gen_run_id();
    cb_manager_run_start(agent->cb_mgr, run_id, user_input);

    Message *user_msg = message_create("user", user_input);
    if (!user_msg) { free(run_id); return NULL; }
    agent_append_message(agent, user_msg);
    agent_save_session(agent);

    for (int iter = 0; iter < agent->max_iterations; iter++)
    {
        if (agent->cancel_requested)
        {
            log_info("agent: cancel requested before LLM call", NULL);
            cb_manager_run_error(agent->cb_mgr, run_id, "cancelled");
            free(run_id);
            return NULL;
        }

        LLMResponse *resp = agent_llm_call(agent);
        if (!resp)
        {
            log_error("agent: llm returned no response", NULL);
            cb_manager_run_error(agent->cb_mgr, run_id, "no response");
            free(run_id);
            return NULL;
        }

        if (resp->content)
        {
            Message *assistant_msg = message_create("assistant", resp->content);
            if (resp->tool_calls_count > 0)
            {
                message_set_tool_calls(assistant_msg, resp->tool_calls, resp->tool_calls_count);
                resp->tool_calls = NULL;
                resp->tool_calls_count = 0;
            }
            agent_append_message(agent, assistant_msg);
            agent_save_session(agent);
        }

        if (!tool_calls_remaining(resp->tool_calls, resp->tool_calls_count))
        {
            agent_save_session(agent);
            agent_generate_title(agent);
            cb_manager_run_end(agent->cb_mgr, run_id, resp->content);
            free(run_id);
            return resp;
        }

        execute_tool_calls(agent, resp->tool_calls, resp->tool_calls_count);
        agent_save_session(agent);
        llm_response_free(resp);
    }

    log_error("agent: max iterations reached", NULL);
    cb_manager_run_error(agent->cb_mgr, run_id, "max iterations");
    free(run_id);
    return NULL;
}

static void null_chunk_handler(const char *chunk, void *userdata)
{
    (void)chunk;
    (void)userdata;
}

LLMResponse *agent_run_streaming(Agent *agent, const char *user_input,
                                 void (*on_chunk)(const char *, void *),
                                 void *userdata)
{
    agent->cancel_requested = 0;

    if (!on_chunk) on_chunk = null_chunk_handler;

    Message *user_msg = message_create("user", user_input);
    if (!user_msg) return NULL;
    agent_append_message(agent, user_msg);
    agent_save_session(agent);

    for (int iter = 0; iter < agent->max_iterations; iter++)
    {
        if (agent->cancel_requested)
        {
            log_info("agent: cancel requested before LLM call", NULL);
            return NULL;
        }

        if (agent->cb && !cb_is_available(agent->cb))
        {
            log_error("circuit breaker open, skipping LLM call", NULL);
            return NULL;
        }

        LLMResponse *resp = agent->provider->chat_streaming(
            agent->provider, agent->messages, agent->messages_count,
            agent->model, agent->temperature, agent->timeout,
            on_chunk, userdata);

        if (resp && agent->cb) cb_record_success(agent->cb);
        else if (!resp && agent->cb) cb_record_failure(agent->cb);

        if (!resp) return NULL;

        if (resp->content)
        {
            Message *assistant_msg = message_create("assistant", resp->content);
            if (resp->tool_calls_count > 0)
            {
                message_set_tool_calls(assistant_msg, resp->tool_calls, resp->tool_calls_count);
                resp->tool_calls = NULL;
                resp->tool_calls_count = 0;
            }
            agent_append_message(agent, assistant_msg);
            agent_save_session(agent);
        }

        if (!tool_calls_remaining(resp->tool_calls, resp->tool_calls_count))
        {
            agent_save_session(agent);
            agent_generate_title(agent);
            return resp;
        }

        execute_tool_calls(agent, resp->tool_calls, resp->tool_calls_count);
        agent_save_session(agent);
        llm_response_free(resp);
    }

    return NULL;
}

void agent_set_session_manager(Agent *agent, SessionManager *sm)
{
    agent->sm = sm;
}

static void agent_save_session(Agent *agent)
{
    if (!agent->sm) return;
    Session *s = NULL;

    if (agent->session_id)
        s = session_manager_load_session(agent->sm, agent->session_id);

    if (!s)
    {
        s = session_create("Echo AI Session");
        if (!s) return;
        if (agent->session_id) free(agent->session_id);
        agent->session_id = str_dup(s->id);
    }

    if (s->messages) message_free_all(s->messages, s->messages_count);
    s->messages = NULL;
    s->messages_count = 0;

    if (agent->messages_count > 0)
    {
        s->messages = calloc((size_t)agent->messages_count, sizeof(Message));
        if (s->messages)
        {
            /* deep-copy: the agent keeps its messages for the live
             * conversation; the session owns the copies it serializes */
            for (int i = 0; i < agent->messages_count; i++)
            {
                if (message_copy(&s->messages[i], &agent->messages[i]) != 0)
                    break;
                s->messages_count = i + 1;
            }
        }
    }

    if (agent->messages_count > 0)
        session_manager_save_session(agent->sm, s);

    session_free(s);
}

void agent_set_approval_callback(Agent *agent,
                                 int (*cb)(const char *, const char *, void *),
                                 void *userdata)
{
    agent->on_approval = cb;
    agent->approval_userdata = userdata;
}

void agent_cancel(Agent *agent)
{
    agent->cancel_requested = 1;
}

void agent_set_metrics(Agent *agent, Metrics *metrics)
{
    agent->metrics = metrics;
}

void agent_set_title_callback(Agent *agent, title_callback cb, void *userdata)
{
    agent->on_title_update = cb;
    agent->title_userdata = userdata;
}

void agent_set_callback_manager(Agent *agent, CallbackManager *mgr)
{
    agent->cb_mgr = mgr;
}
