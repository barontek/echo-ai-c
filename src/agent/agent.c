/*
 * agent.c - the agent facade: lifecycle, message append, session
 * persistence, and the set_* configuration surface. The run loop
 * lives in agent_run.c, tool execution in agent_tools.c, prompt
 * building in agent_prompt.c, title generation in agent_title.c,
 * and summarization in agent_summarize.c; shared internal contracts
 * in agent_internal.h.
 * Depends on: tools/registry, session_manager, logging, string_utils.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "agent.h"
#include "agent_internal.h"
#include "agent_prompt.h"
#include "agent_tools.h"
#include "agent_title.h"
#include "agent_summarize.h"
#include "agent_run.h"
#include "../tools/registry.h"
#include "../tools/tool.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"
#include "../session/session_manager.h"

#ifdef AGENT_TEST
/* Test-only seams for tests/agent/test_agent_save.c: expose the tool-call
 * executor and inject realloc failures into the message-array growth so
 * the OOM paths (message_create NULL, append failure) are exercisable.
 * Only the test target defines AGENT_TEST; production builds never see
 * the shim. The shim is defined before the #define so its own body calls
 * the real realloc. */
static int agent_test_realloc_counter = 0;
static int agent_test_realloc_fail_at = -1;
void *agent_test_realloc(void *ptr, size_t size)
{
    agent_test_realloc_counter++;
    if (agent_test_realloc_counter == agent_test_realloc_fail_at) return NULL;
    return realloc(ptr, size);
}
#define realloc agent_test_realloc
void agent_test_set_realloc_fail(int nth)
{
    agent_test_realloc_counter = 0;
    agent_test_realloc_fail_at = nth;
}
#endif



Agent *agent_create(const AgentConfig *cfg)
{
    Agent *agent = calloc(1, sizeof(Agent));
    if (!agent) return NULL;

    agent->openai_auth = cfg->openai_auth;
    if (cfg->openai_auth && strcmp(cfg->provider, "openai") == 0)
        agent->provider = get_provider_with_auth(cfg->provider, cfg->model,
                                                 cfg->base_url, cfg->api_token,
                                                 cfg->num_ctx, cfg->keep_alive_secs,
                                                 cfg->effort,
                                                 cfg->openai_auth);
    else
        agent->provider = get_provider(cfg->provider, cfg->model,
                                       cfg->base_url, cfg->api_token,
                                       cfg->num_ctx, cfg->keep_alive_secs,
                                       cfg->effort);
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

    agent->effort = cfg->effort && cfg->effort[0] ? str_dup(cfg->effort) : NULL;
    if (cfg->effort && cfg->effort[0] && !agent->effort)
    {
        log_error("agent_create: str_dup failed", "field", "effort", NULL);
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
    agent->max_tool_result_chars = cfg->max_tool_result_chars;
    agent->parallel_tool_exec = cfg->parallel_tool_exec;
    atomic_store(&agent->cancel_requested, 0);
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
    free(agent->effort);
    cb_destroy(agent->cb);
    free(agent);
}

int agent_append_message(Agent *agent, Message *msg)
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

double time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
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
        s = session_manager_load_session_nolock_alloc(agent->sm, agent->session_id);

    if (!s)
    {
        if (agent->session_id)
            log_warn("agent_save_session: existing session not found, minting new id",
                     "old_id", agent->session_id, NULL);
        s = session_create("Echo AI Session");
        if (!s) {
            session_manager_unlock(agent->sm);
            return;
        }
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

        s->messages = calloc((size_t)save_count, sizeof(Message)); // NOLINT(clang-analyzer-optin.portability.UnixAPI)
        if (!s->messages)
        {
            log_error("agent_save_session: OOM allocating messages", NULL);
        }
        else
        {
            int si = 0;
            for (int i = 0; i < agent->messages_count && si < save_count; i++)
            {
                if (strcmp(agent->messages[i].role, "system") == 0)
                    continue;
                if (message_copy(&s->messages[si], &agent->messages[i]) != 0)
                {
                    /* C11: mid-save failure used to persist silently
                     * truncated history. */
                    log_error("agent_save_session: message copy failed", NULL);
                    break;
                }
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
    /* _Atomic per agent.h's cross-thread contract: a plain volatile write
     * from another thread would be data-race UB (benign in practice, but
     * the header promises "safe to call from another thread"). */
    atomic_store(&agent->cancel_requested, 1);
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

int agent_set_model(Agent *agent, const char *model)
{
    if (!agent || !model) return -1;
    char *model_dup = str_dup(model);
    if (!model_dup)
    {
        log_error("agent_set_model: OOM duplicating model name", NULL);
        return -1;
    }
    /* Only swap after the copy succeeded, so an OOM leaves the previous
     * model installed instead of silently clearing it. */
    free(agent->model);
    agent->model = model_dup;
    return 0;
}

int agent_set_provider(Agent *agent, const char *provider, const char *base_url,
                       const char *api_token, int num_ctx, int keep_alive_secs,
                       const char *effort)
{
    if (!agent || !provider || !provider[0]) return -1;

    /* Same provider AND same effort: nothing to rebuild; the model is
     * switched separately via agent_set_model. An effort change must
     * rebuild the provider because effort is baked in at creation time
     * (the OpenAI provider embeds it in every request body). */
    if (agent->provider_name && strcmp(agent->provider_name, provider) == 0)
    {
        const char *cur = agent->effort ? agent->effort : "";
        const char *wanted = effort ? effort : "";
        if (strcmp(cur, wanted) == 0)
            return 0;
    }

    /* Build the replacement first so a failure leaves the old provider
     * untouched and the connection usable. */
    LLMProvider *replacement = NULL;
    if (agent->openai_auth && strcmp(provider, "openai") == 0)
        replacement = get_provider_with_auth(provider,
                                             agent->model ? agent->model : "",
                                             base_url, api_token, num_ctx,
                                             keep_alive_secs, effort,
                                             agent->openai_auth);
    else
        replacement = get_provider(provider, agent->model ? agent->model : "",
                                   base_url, api_token, num_ctx, keep_alive_secs,
                                   effort);
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
    char *new_effort = effort && effort[0] ? str_dup(effort) : NULL;
    if (effort && effort[0] && !new_effort)
    {
        free(new_name);
        free(new_token);
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
    free(agent->effort);
    agent->effort = new_effort;
    return 0;
}
