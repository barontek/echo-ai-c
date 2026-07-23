#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int execute_tool_calls(Agent *agent, ToolCall *calls, int count)
{
    for (int i = 0; i < count; i++)
    {
        Tool *tool = registry_get(calls[i].name);
        if (!tool)
        {
            Message *err_msg = message_create("tool", "tool not found");
            err_msg->tool_call_id = str_dup(calls[i].id ? calls[i].id : "");
            err_msg->tool_name = str_dup(calls[i].name);
            err_msg->error_category = str_dup("tool_not_found");
            agent_append_message(agent, err_msg);
            continue;
        }

        if (agent->on_approval)
        {
            char *args_str = calls[i].arguments ? calls[i].arguments : "{}";
            int ok = agent->on_approval(calls[i].name, args_str, agent->approval_userdata);
            if (!ok)
            {
                Message *err_msg = message_create("tool", "tool call denied");
                err_msg->tool_call_id = str_dup(calls[i].id ? calls[i].id : "");
                err_msg->tool_name = str_dup(calls[i].name);
                err_msg->error_category = str_dup("denied");
                agent_append_message(agent, err_msg);
                continue;
            }
        }

        char *args_str = calls[i].arguments ? calls[i].arguments : "{}";
        ToolResult *result = tool->execute(tool, args_str);
        if (!result)
            result = tool_result_error("tool returned no result", "execution_error");

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
        tool_result_free(result);
    }

    return 0;
}

static LLMResponse *agent_llm_call(Agent *agent)
{
    if (agent->cb && !cb_is_available(agent->cb))
    {
        log_error("circuit breaker open, skipping LLM call", NULL);
        return NULL;
    }

    int current_messages = agent->messages_count;
    Message *ctx_msgs = apply_context_window(agent->messages, &current_messages,
                                             agent->max_context_messages,
                                             agent->max_context_chars);
    if (ctx_msgs != agent->messages)
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
        }
        free(agent->messages);
        agent->messages = ctx_msgs;
        agent->messages_count = current_messages;
    }

    LLMResponse *resp = agent->provider->chat(
        agent->provider, agent->messages, agent->messages_count,
        agent->model, agent->temperature, agent->timeout);

    if (resp && agent->cb) cb_record_success(agent->cb);
    else if (!resp && agent->cb) cb_record_failure(agent->cb);

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

LLMResponse *agent_run(Agent *agent, const char *user_input)
{
    if (agent->cancel_requested) return NULL;

    Message *user_msg = message_create("user", user_input);
    if (!user_msg) return NULL;
    agent_append_message(agent, user_msg);
    agent_save_session(agent);

    for (int iter = 0; iter < agent->max_iterations; iter++)
    {
        LLMResponse *resp = agent_llm_call(agent);
        if (!resp)
        {
            log_error("agent: llm returned no response", NULL);
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
            return resp;
        }

        execute_tool_calls(agent, resp->tool_calls, resp->tool_calls_count);
        agent_save_session(agent);
        llm_response_free(resp);
    }

    log_error("agent: max iterations reached", NULL);
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
    if (agent->cancel_requested) return NULL;

    if (!on_chunk) on_chunk = null_chunk_handler;

    Message *user_msg = message_create("user", user_input);
    if (!user_msg) return NULL;
    agent_append_message(agent, user_msg);
    agent_save_session(agent);

    for (int iter = 0; iter < agent->max_iterations; iter++)
    {
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
    if (!agent->session_id) return;
    Session *s = session_manager_load_session(agent->sm, agent->session_id);
    if (!s)
    {
        s = session_manager_create_session(agent->sm, "Echo AI Session");
        if (!s) return;
        free(agent->session_id);
        agent->session_id = str_dup(s->id);
    }

    if (s->messages) message_free_all(s->messages, s->messages_count);
    s->messages = NULL;
    s->messages_count = 0;

    if (agent->messages_count > 0)
    {
        s->messages = calloc(agent->messages_count, sizeof(Message));
        if (s->messages)
        {
            s->messages_count = agent->messages_count;
            for (int i = 0; i < agent->messages_count; i++)
            {
                s->messages[i] = agent->messages[i];
                agent->messages[i].role = NULL;
                agent->messages[i].content = NULL;
                agent->messages[i].id = NULL;
                agent->messages[i].tool_call_id = NULL;
                agent->messages[i].tool_name = NULL;
                agent->messages[i].error_category = NULL;
                agent->messages[i].thinking = NULL;
                agent->messages[i].tool_calls = NULL;
                agent->messages[i].tool_calls_count = 0;
            }
        }
    }

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
