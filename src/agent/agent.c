#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#define SYSTEM_OS "Linux"
#elif defined(__APPLE__)
#define SYSTEM_OS "macOS"
#elif defined(_WIN32)
#define SYSTEM_OS "Windows"
#else
#define SYSTEM_OS "Unknown"
#endif

#include "agent.h"
#include "context.h"
#include "../tools/registry.h"
#include "../tools/tool.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"
#include "../session/session_manager.h"
#include "../session/memory.h"

Agent *agent_create(const AgentConfig *cfg)
{
    Agent *agent = calloc(1, sizeof(Agent));
    if (!agent) return NULL;

    agent->provider = get_provider(cfg->provider, cfg->model,
                                   cfg->base_url, cfg->api_token,
                                   cfg->num_ctx, cfg->keep_alive_secs);
    if (!agent->provider)
    {
        log_error("failed to create provider", "name", cfg->provider, NULL);
        free(agent);
        return NULL;
    }

    agent->provider_name = str_dup(cfg->provider);
    if (!agent->provider_name)
    {
        log_error("agent_create: str_dup failed", "field", "provider_name", NULL);
        agent_destroy(agent);
        return NULL;
    }

    agent->provider_token = str_dup(cfg->api_token ? cfg->api_token : "");
    if (!agent->provider_token)
    {
        log_error("agent_create: str_dup failed", "field", "provider_token", NULL);
        agent_destroy(agent);
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
        /* C15: route through message_clear so adding a Message field only
         * requires updating one place. Also closes a real leak here — the
         * inline loop in agent_free originally did NOT call tool_call_free on
         * each message's tool_calls inner array, leaking every tool_call's
         * strings + the tool_calls buffer itself whenever the agent owned
         * tool-call-bearing messages. */
        for (int i = 0; i < agent->messages_count; i++)
            message_clear(&agent->messages[i]);
        free(agent->messages);
    }
    free(agent->model);
    free(agent->provider_name);
    free(agent->provider_token);
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

void agent_save_session(Agent *agent);
static void agent_generate_title(Agent *agent);
static void agent_perform_summarization(Agent *agent, int original_count);

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

        if (agent->safety && safety_needs_approval(agent->safety, calls[i].name))
        {
            int ok = agent->on_approval
                         ? agent->on_approval(calls[i].name, args_str,
                                              agent->approval_userdata)
                         : 0;
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

        if (agent->on_tool_start)
            agent->on_tool_start(tname, args_str, agent->tool_start_userdata);

        registry_set_ask_user_callback(agent->on_ask_user,
                                       agent->ask_user_userdata);
        ToolResult *result = tool->execute(tool, args_str);
        registry_set_ask_user_callback(NULL, NULL);
        if (!result)
            result = tool_result_error("tool returned no result", "execution_error");

        free(calls[i].result_content);
        free(calls[i].result_error);
        calls[i].result_content = str_dup(result->content ? result->content : "");
        calls[i].result_error = str_dup(result->error ? result->error : "");

        if (agent->on_tool_end)
            agent->on_tool_end(tname, calls[i].id,
                               calls[i].result_content, calls[i].result_error,
                               agent->tool_end_userdata);

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
    char cwd_buf[4096];
    const char *cwd = getcwd(cwd_buf, sizeof(cwd_buf));
    if (!cwd) cwd = ".";

    time_t now = time(NULL);
    /* D3: thread-safe localtime_r over thread-unsafe localtime. */
    struct tm tm_storage;
    struct tm *tm_info = localtime_r(&now, &tm_storage);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    char context_buf[512];
    snprintf(context_buf, sizeof(context_buf),
             "\n\n[System Context]\nOS: " SYSTEM_OS "\n"
             "Current Working Directory: %s\nCurrent Time: %s\n",
             cwd, time_buf);

    char *mem_buf = NULL;
    if (agent->sm && agent->sm->db)
    {
        int mem_count = 0;
        MemoryFact *memories = memory_list_all(agent->sm->db, &mem_count);
        if (memories && mem_count > 0)
        {
            size_t mbsz = 512;
            mem_buf = malloc(mbsz);
            if (mem_buf)
            {
                size_t pos = 0;
                int w = snprintf(mem_buf, mbsz, "\n\n[Persistent Memory]\n");
                if (w > 0) pos = (size_t)w;
                int limit = mem_count < 64 ? mem_count : 64;
                for (int i = 0; i < limit; i++)
                {
                    size_t needed = pos + strlen(memories[i].key)
                                    + strlen(memories[i].value) + 12;
                    if (needed >= mbsz)
                    {
                        mbsz = needed + 256;
                        char *newbuf = realloc(mem_buf, mbsz);
                        if (!newbuf) { free(mem_buf); mem_buf = NULL; break; }
                        mem_buf = newbuf;
                    }
                    w = snprintf(mem_buf + pos, mbsz - pos,
                                 "%s = %s\n", memories[i].key, memories[i].value);
                    if (w > 0) pos += (size_t)w;
                }
            }
        }
        memory_facts_free(memories, mem_count);
    }

    const char *base = agent->system_prompt ? agent->system_prompt : "";
    if (agent->context_summary)
    {
        if (asprintf(out, "%s%s%s\n\nPrevious conversation summary: %s",
                     base, context_buf, mem_buf ? mem_buf : "",
                     agent->context_summary) < 0)
            { free(mem_buf); return -1; }
    }
    else
    {
        if (asprintf(out, "%s%s%s",
                     base, context_buf, mem_buf ? mem_buf : "") < 0)
            { free(mem_buf); return -1; }
    }
    free(mem_buf);
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
        int dropped = original_count - current_messages;
        if (dropped > 0) agent_perform_summarization(agent, dropped);

        for (int i = 0; i < agent->messages_count; i++)
            message_clear(&agent->messages[i]);
        free(agent->messages);
        agent->messages = ctx_msgs;
        agent->messages_count = current_messages;
    }

    double start = time_sec();
    double llm_buckets[] = {0.5, 1, 2, 5, 10, 30, 60, 120};

    char *tools_json = registry_schemas_json();
    LLMResponse *resp = agent->provider->chat(
        agent->provider, agent->messages, agent->messages_count,
        agent->model, agent->temperature, agent->timeout,
        tools_json);
    free(tools_json);

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

static void
attach_tool_calls_to_resp(LLMResponse *resp, const Message *msgs, int count)
{
    if (!resp || resp->tool_calls_count > 0) return;

    for (int i = count - 1; i >= 0; i--)
    {
        /* D6: stop at the current turn's user message so tool_calls
         * from previous turns are never attached to a later response. */
        if (msgs[i].role && strcmp(msgs[i].role, "user") == 0) return;
        if (msgs[i].role && strcmp(msgs[i].role, "assistant") != 0) continue;
        if (msgs[i].tool_calls_count == 0) continue;

        int n = msgs[i].tool_calls_count;
        resp->tool_calls = calloc((size_t)n, sizeof(ToolCall));
        if (!resp->tool_calls) return;

        resp->tool_calls_count = n;
        for (int j = 0; j < n; j++)
        {
            resp->tool_calls[j].name = str_dup(msgs[i].tool_calls[j].name);
            resp->tool_calls[j].arguments = str_dup(msgs[i].tool_calls[j].arguments);
            resp->tool_calls[j].id = str_dup(msgs[i].tool_calls[j].id);
            resp->tool_calls[j].result_content = str_dup(msgs[i].tool_calls[j].result_content);
            resp->tool_calls[j].result_error = str_dup(msgs[i].tool_calls[j].result_error);
        }
        return;
    }
}

/* strip <think>...</think> blocks (the non-overlapping first match only), then
 * return a fresh malloc'd copy.  Returns the original unchanged if no tag found. */
static char *strip_think_tags(const char *str)
{
    if (!str) return NULL;

    const char *open_tag  = "<think>";
    const char *close_tag = "</think>";
    size_t open_len  = strlen(open_tag);
    size_t close_len = strlen(close_tag);

    const char *open = strstr(str, open_tag);
    if (!open) return str_dup(str);

    const char *close = strstr(open + open_len, close_tag);
    if (!close) return str_dup(str);

    size_t before     = (size_t)(open - str);
    size_t suffix_off = (size_t)(close + close_len - str);
    size_t after      = strlen(str) - suffix_off;
    size_t result_len = before + after;

    char *result = malloc(result_len + 1);
    if (!result) return NULL;
    if (before > 0) memcpy(result, str, before);
    if (after  > 0) memcpy(result + before, str + suffix_off, after);
    result[result_len] = '\0';
    return result;
}

static void agent_generate_title(Agent *agent)
{
    if (!agent || !agent->provider) return;
    if (!agent->sm || !agent->session_id) return;
    if (agent->messages_count == 0) return;

    Session *s = session_manager_load_session(agent->sm, agent->session_id);
    if (!s || s->title_generation_attempted) { if (s) session_free(s); return; }
    session_free(s);

    s = session_manager_load_session(agent->sm, agent->session_id);
    if (!s) return;
    s->title_generation_attempted = 1;
    session_manager_save_session(agent->sm, s);
    session_free(s);

    /* find first user message — matching Python version's approach:
     * only the first user request, not the full conversation.
     * full-conversation excerpts confuse small models into producing
     * hallucinated placeholder titles like "(Waiting for ...)" */
    const char *first_user_msg = NULL;
    for (int i = 0; i < agent->messages_count; i++)
    {
        if (agent->messages[i].role && strcmp(agent->messages[i].role, "user") == 0
            && agent->messages[i].content && agent->messages[i].content[0])
        {
            first_user_msg = agent->messages[i].content;
            break;
        }
    }
    if (!first_user_msg) return;

    /* fallback: first 30 chars of user message, with "..." if truncated */
    char *fallback = NULL;
    size_t fblen = strlen(first_user_msg);
    if (fblen <= 30) {
        fallback = str_dup(first_user_msg);
    } else {
        if (asprintf(&fallback, "%.30s...", first_user_msg) < 0)
            fallback = NULL;
    }
    if (!fallback) return;

    /* prompt matching Python version — single user message, no system prompt */
    char *prompt = NULL;
    if (asprintf(&prompt,
                 "Summarize the following user request into a very short, "
                 "descriptive title (max 5 words). "
                 "Do not use quotes or a period.\n\n"
                 "User request: %s",
                 first_user_msg) < 0)
    {
        free(fallback);
        return;
    }

    log_info("title prompt", "text", prompt, NULL);

    Message title_msg;
    memset(&title_msg, 0, sizeof(title_msg));
    title_msg.role    = str_dup("user");
    title_msg.content = prompt;
    if (!title_msg.role)
    {
        free(prompt);
        free(fallback);
        return;
    }

    LLMResponse *resp = agent->provider->chat(
        agent->provider, &title_msg, 1,
        agent->model, 0.3, 30, NULL);

    free(title_msg.role);
    free(title_msg.content);

    char *final_title = NULL;

    if (resp && resp->content)
    {
        log_info("title from model", "title", resp->content, NULL);

        char *raw = str_dup(resp->content);
        llm_response_free(resp);

        if (raw)
        {
            char *t = str_trim(raw);
            if (t && t[0])
            {
                char *no_think = strip_think_tags(t);
                if (no_think)
                {
                    char *c = str_trim(no_think);
                    if (c && c[0])
                    {
                        /* strip leading / trailing double-quotes */
                        size_t clen = strlen(c);
                        if ((c[0] == '"' && c[clen - 1] == '"')
                            || (c[0] == '\'' && c[clen - 1] == '\''))
                        {
                            c[clen - 1] = '\0';
                            memmove(c, c + 1, clen);
                        }
                        if (c[0]) final_title = str_dup(c);
                    }
                    free(no_think);
                }
            }
            free(raw);
        }
    }
    else if (resp)
    {
        llm_response_free(resp);
    }

    /* fall back to truncated first user message if LLM produced nothing */
    if (!final_title)
        final_title = str_dup(fallback);

    Session *s2 = session_manager_load_session(agent->sm, agent->session_id);
    if (s2)
    {
        free(s2->title);
        s2->title = str_dup(final_title);
        session_manager_save_session(agent->sm, s2);
        session_free(s2);

        if (agent->on_title_update)
            agent->on_title_update(agent->session_id, final_title, agent->title_userdata);
    }

    free(final_title);
    free(fallback);
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
        agent->model, 0.3, 30, NULL);

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

/* D4: process-wide run id counter. Increments non-atomically. The single
 * shared `Agent` (per D2) is mutated by every WS client; in a true
 * multi-client overlap, two threads could read the same `run_counter` and
 * both increment to the same value. The minter IDs (`run_<n>`) are only
 * used for metrics/breaker tracking, NOT for DB state attribution — so
 * a duplicate is benign (the metrics series just sees the same run_id
 * twice, no DB write is keyed on it). For the single-process libuv loop
 * the practical risk is essentially zero (the counter increments inside
 * `gen_run_id` called serially from `agent_run_streaming`). If you ever
 * lift D2's single-Agent restriction, switch this to C11 `<stdatomic.h>`
 * `atomic_fetch_add` first. */
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

        int has_tool_calls = tool_calls_remaining(resp->tool_calls, resp->tool_calls_count);
        ToolCall *exec_calls = resp->tool_calls;
        int exec_count = resp->tool_calls_count;

        if (resp->content)
        {
            Message *assistant_msg = message_create("assistant", resp->content);
            if (assistant_msg && resp->thinking)
                assistant_msg->thinking = str_dup(resp->thinking);
            if (has_tool_calls)
            {
                message_set_tool_calls(assistant_msg, resp->tool_calls, resp->tool_calls_count);
                resp->tool_calls = NULL; /* ownership transferred */
                resp->tool_calls_count = 0;
            }
            agent_append_message(agent, assistant_msg);
            agent_save_session(agent);
        }

        if (!has_tool_calls)
        {
            attach_tool_calls_to_resp(resp, agent->messages, agent->messages_count);
            agent_save_session(agent);
            agent_generate_title(agent);
            cb_manager_run_end(agent->cb_mgr, run_id, resp->content);
            free(run_id);
            return resp;
        }

        execute_tool_calls(agent, exec_calls, exec_count);
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

        inject_system_with_summary(agent);

        char *tools_json = registry_schemas_json();
        LLMResponse *resp = agent->provider->chat_streaming(
            agent->provider, agent->messages, agent->messages_count,
            agent->model, agent->temperature, agent->timeout,
            on_chunk, userdata,
            tools_json);
        free(tools_json);

        if (resp && agent->cb) cb_record_success(agent->cb);
        else if (!resp && agent->cb) cb_record_failure(agent->cb);

        if (!resp) return NULL;

        int has_tool_calls = tool_calls_remaining(resp->tool_calls, resp->tool_calls_count);
        ToolCall *exec_calls = resp->tool_calls;
        int exec_count = resp->tool_calls_count;

        if (resp->content)
        {
            Message *assistant_msg = message_create("assistant", resp->content);
            if (assistant_msg && resp->thinking)
                assistant_msg->thinking = str_dup(resp->thinking);
            if (has_tool_calls)
            {
                message_set_tool_calls(assistant_msg, resp->tool_calls, resp->tool_calls_count);
                resp->tool_calls = NULL; /* ownership transferred */
                resp->tool_calls_count = 0;
            }
            agent_append_message(agent, assistant_msg);
            agent_save_session(agent);
        }

        if (!has_tool_calls)
        {
            attach_tool_calls_to_resp(resp, agent->messages, agent->messages_count);
            agent_save_session(agent);
            agent_generate_title(agent);
            return resp;
        }

        execute_tool_calls(agent, exec_calls, exec_count);
        agent_save_session(agent);
        llm_response_free(resp);
    }

    return NULL;
}

void agent_set_session_manager(Agent *agent, SessionManager *sm)
{
    agent->sm = sm;
}

void agent_save_session(Agent *agent)
{
    if (!agent->sm) return;

    /* C10: hold sm->lock across load→mutate→save so no
     * session_manager_log_event or concurrent agent_save_session can
     * interleave. The _nolock call and all helper calls
     * (session_create, message_copy, message_free_all, str_dup) make zero
     * re-entry into any session_manager_* API — audited locally from the
     * diff. */
    session_manager_lock(agent->sm);

    Session *s = NULL;

    if (agent->session_id)
        s = session_manager_load_session_nolock(agent->sm, agent->session_id);

    if (!s)
    {
        if (agent->session_id)
            log_warn("agent_save_session: existing session not found, minting new id",
                     "old_id", agent->session_id, NULL);
        s = session_create("Echo AI Session");
        if (!s) { session_manager_unlock(agent->sm); return; }
        free(agent->session_id);
        agent->session_id = str_dup(s->id);
    }

    if (s->messages) message_free_all(s->messages, s->messages_count);
    s->messages = NULL;
    s->messages_count = 0;

    if (agent->messages_count > 0)
    {
        int save_count = 0;
        for (int i = 0; i < agent->messages_count; i++)
            if (strcmp(agent->messages[i].role, "system") != 0)
                save_count++;

        s->messages = calloc((size_t)save_count, sizeof(Message));
        if (s->messages)
        {
            int si = 0;
            for (int i = 0; i < agent->messages_count && si < save_count; i++)
            {
                if (strcmp(agent->messages[i].role, "system") == 0)
                    continue;
                if (message_copy(&s->messages[si], &agent->messages[i]) != 0)
                    break;
                si++;
            }
            s->messages_count = si;
        }
    }

    /* C10: use the _nolock save variant — we already hold sm->lock */
    session_manager_save_session_nolock(agent->sm, s);

    session_manager_unlock(agent->sm);
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

void agent_set_tool_start_callback(Agent *agent, tool_start_callback cb, void *userdata)
{
    agent->on_tool_start = cb;
    agent->tool_start_userdata = userdata;
}

void agent_set_tool_end_callback(Agent *agent, tool_end_callback cb, void *userdata)
{
    agent->on_tool_end = cb;
    agent->tool_end_userdata = userdata;
}

void agent_set_ask_user_callback(Agent *agent, ask_user_callback cb, void *userdata)
{
    if (!agent) return;
    agent->on_ask_user = cb;
    agent->ask_user_userdata = userdata;
}

void agent_set_safety(Agent *agent, SafetyConfig *safety)
{
    agent->safety = safety;
}

void agent_set_callback_manager(Agent *agent, CallbackManager *mgr)
{
    agent->cb_mgr = mgr;
}

void agent_set_model(Agent *agent, const char *model)
{
    if (!agent || !model) return;
    free(agent->model);
    agent->model = str_dup(model);
}

int agent_set_provider(Agent *agent, const char *provider, const char *base_url,
                       const char *api_token, int num_ctx, int keep_alive_secs)
{
    if (!agent || !provider || !provider[0]) return -1;

    /* Same provider: nothing to rebuild; the model is switched separately
     * via agent_set_model. */
    if (agent->provider_name && strcmp(agent->provider_name, provider) == 0)
        return 0;

    /* Build the replacement first so a failure leaves the old provider
     * untouched and the connection usable. */
    LLMProvider *replacement = get_provider(provider,
                                            agent->model ? agent->model : "",
                                            base_url, api_token,
                                            num_ctx, keep_alive_secs);
    if (!replacement)
    {
        log_error("failed to create provider", "name", provider, NULL);
        return -1;
    }
    char *new_name = str_dup(provider);
    if (!new_name)
    {
        replacement->destroy(replacement);
        return -1;
    }
    char *new_token = str_dup(api_token ? api_token : "");
    if (!new_token)
    {
        free(new_name);
        replacement->destroy(replacement);
        return -1;
    }

    if (agent->provider)
        agent->provider->destroy(agent->provider);
    agent->provider = replacement;
    free(agent->provider_name);
    agent->provider_name = new_name;
    free(agent->provider_token);
    agent->provider_token = new_token;
    return 0;
}
