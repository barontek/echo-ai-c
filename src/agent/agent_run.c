/*
 * agent_run.c - the agent run loop: circuit breaker, context
 * windowing, LLM calls, tool turns, title generation, and
 * session persistence for buffered and streaming runs.
 * Depends on: context, tools/registry, message, metrics.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "agent.h"
#include "agent_internal.h"
#include "agent_run.h"
#include "agent_prompt.h"
#include "agent_tools.h"
#include "agent_title.h"
#include "agent_summarize.h"
#include "context.h"
#include "../tools/registry.h"
#include "../tools/tool.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"


/* D4: process-wide run id counter. Increments non-atomically. The single
 * shared `Agent` (per D2) is mutated by every WS client; in a true
 * multi-client overlap, two threads could read the same `run_counter` and
 * both increment to the same value. The minter IDs (`run_<n>`) are only
 * used for metrics/breaker tracking, NOT for DB state attribution — so
 * a duplicate is benign (the metrics series just sees the same run_id
 * twice, no DB write is keyed on it). For the single-process libuv loop
 * the practical risk is essentially zero (the counter increments inside
 * `gen_run_id` called serially from `agent_run_streaming_new`). If you ever
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

static void null_chunk_handler(const char *chunk, void *userdata)
{
    (void)chunk;
    (void)userdata;
}

static LLMResponse *agent_run_loop(Agent *agent,
                                   void (*on_chunk)(const char *, void *),
                                   void *userdata)
{
    if (!on_chunk) on_chunk = null_chunk_handler;

    for (int iter = 0; iter < agent->max_iterations; iter++)
    {
        if (atomic_load(&agent->cancel_requested))
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
            if (!assistant_msg)
            {
                llm_response_free(resp);
                return NULL;
            }
            if (assistant_msg && resp->thinking)
                assistant_msg->thinking = str_dup(resp->thinking);
            if (assistant_msg && resp->phase)
                assistant_msg->phase = str_dup(resp->phase);
            if (assistant_msg && resp->provider_state)
                assistant_msg->provider_state = str_dup(resp->provider_state);
            if ((resp->thinking && !assistant_msg->thinking) ||
                (resp->phase && !assistant_msg->phase) ||
                (resp->provider_state && !assistant_msg->provider_state))
            {
                message_free(assistant_msg);
                llm_response_free(resp);
                return NULL;
            }
            if (has_tool_calls)
            {
                message_set_tool_calls(assistant_msg, resp->tool_calls, resp->tool_calls_count);
                resp->tool_calls = NULL; /* ownership transferred */
                resp->tool_calls_count = 0;
            }
            if (agent_append_message(agent, assistant_msg) >= 0)
                free(assistant_msg); /* struct only: fields moved into the array */
            else
            {
                log_error("agent_run: OOM appending assistant message", NULL);
                message_free(assistant_msg);
            }
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

LLMResponse *agent_run_new(Agent *agent, const char *user_input)
{
    atomic_store(&agent->cancel_requested, 0);

    char *run_id = gen_run_id();
    cb_manager_run_start(agent->cb_mgr, run_id, user_input);

    Message *user_msg = message_create("user", user_input);
    if (!user_msg) {
        free(run_id);
        return NULL;
    }
    if (agent_append_message(agent, user_msg) >= 0)
        free(user_msg); /* struct only: fields moved into the array */
    else
    {
        log_error("agent_run: OOM appending user message", NULL);
        message_free(user_msg);
    }
    agent_save_session(agent);

    for (int iter = 0; iter < agent->max_iterations; iter++)
    {
        if (atomic_load(&agent->cancel_requested))
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
            if (!assistant_msg)
            {
                llm_response_free(resp);
                free(run_id);
                return NULL;
            }
            if (assistant_msg && resp->thinking)
                assistant_msg->thinking = str_dup(resp->thinking);
            if (assistant_msg && resp->phase)
                assistant_msg->phase = str_dup(resp->phase);
            if (assistant_msg && resp->provider_state)
                assistant_msg->provider_state = str_dup(resp->provider_state);
            if ((resp->thinking && !assistant_msg->thinking) ||
                (resp->phase && !assistant_msg->phase) ||
                (resp->provider_state && !assistant_msg->provider_state))
            {
                message_free(assistant_msg);
                llm_response_free(resp);
                free(run_id);
                return NULL;
            }
            if (has_tool_calls)
            {
                message_set_tool_calls(assistant_msg, resp->tool_calls, resp->tool_calls_count);
                resp->tool_calls = NULL; /* ownership transferred */
                resp->tool_calls_count = 0;
            }
            if (agent_append_message(agent, assistant_msg) >= 0)
                free(assistant_msg); /* struct only: fields moved into the array */
            else
            {
                log_error("agent_run: OOM appending assistant message", NULL);
                message_free(assistant_msg);
            }
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

LLMResponse *agent_run_streaming_new(Agent *agent, const char *user_input,
                                 void (*on_chunk)(const char *, void *),
                                 void *userdata)
{
    atomic_store(&agent->cancel_requested, 0);

    Message *user_msg = message_create("user", user_input);
    if (!user_msg) return NULL;
    if (agent_append_message(agent, user_msg) >= 0)
        free(user_msg); /* struct only: fields moved into the array */
    else
    {
        log_error("agent_run: OOM appending user message", NULL);
        message_free(user_msg);
    }
    agent_save_session(agent);

    return agent_run_loop(agent, on_chunk, userdata);
}

LLMResponse *agent_run_streaming_context_new(Agent *agent,
                                         void (*on_chunk)(const char *, void *),
                                         void *userdata)
{
    atomic_store(&agent->cancel_requested, 0);
    return agent_run_loop(agent, on_chunk, userdata);
}
