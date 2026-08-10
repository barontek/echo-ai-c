/*
 * agent.h - the agent loop: LLM calls, tool execution, context windowing,
 * session persistence, and title generation for one conversation.
 * Depends on: message.h, provider.h, openai_oauth.h, session_manager.h,
 * safety.h, circuit_breaker.h, callbacks.h, metrics.h.
 */

#ifndef ECHO_AGENT_H
#define ECHO_AGENT_H

#include "message.h"
#include "../llm/provider.h"
#include "../llm/openai_oauth.h"
#include "../session/session_manager.h"
#include "../safety/safety.h"
#include "../utils/circuit_breaker.h"
#include "../utils/callbacks.h"
#include "../utils/metrics.h"

typedef struct {
    const char *provider;
    const char *model;
    const char *base_url;
    const char *api_token;
    OpenAIOAuth *openai_auth;
    const char *system_prompt;
    double temperature;
    int timeout;
    int max_iterations;
    int max_context_messages;
    int max_context_chars;
    int max_tool_result_chars;
    int num_ctx;
    int keep_alive_secs;
    int parallel_tool_exec;
    /* Reasoning-effort hint for providers that support it (values are
     * provider-specific; see provider_effort_options); NULL = provider
     * default. Borrowed from conf. */
    const char *effort;
} AgentConfig;

typedef void (*title_callback)(const char *session_id, const char *title, void *userdata);
typedef void (*tool_start_callback)(const char *tool_name, const char *arguments, void *userdata);
typedef void (*tool_end_callback)(const char *tool_name, const char *tool_call_id,
                                  const char *result_content, const char *result_error,
                                  void *userdata);
typedef char *(*ask_user_callback)(const char *question, void *userdata);

typedef struct {
    LLMProvider *provider;
    char *provider_name; /* canonical factory name of provider ("ollama"/"openai") */
    char *provider_token; /* API token for the current provider, owned by the agent */
    OpenAIOAuth *openai_auth; /* borrowed shared OAuth state */
    Message *messages;
    int messages_count;
    char *model;
    char *system_prompt;
    char *session_id;
    double temperature;
    int timeout;
    int max_iterations;
    int max_context_messages;
    int max_context_chars;
    int max_tool_result_chars;
    int parallel_tool_exec;
    char *effort; /* owned; NULL = provider default */
    volatile int cancel_requested;
    SessionManager *sm;
    CircuitBreaker *cb;
    CallbackManager *cb_mgr;
    Metrics *metrics;
    int (*on_approval)(const char *tool_name, const char *arguments, void *userdata);
    void *approval_userdata;
    char *context_summary;
    title_callback on_title_update;
    void *title_userdata;
    tool_start_callback on_tool_start;
    void *tool_start_userdata;
    tool_end_callback on_tool_end;
    void *tool_end_userdata;
    ask_user_callback on_ask_user;
    void *ask_user_userdata;
    SafetyConfig *safety;
} Agent;

/**
 * agent_create - construct an agent and its LLM provider
 * @cfg: configuration to copy; string fields are borrowed for the
 *   duration of the call and the agent keeps its own copies. Must be
 *   non-NULL.
 *
 * Builds the provider via get_provider() (or get_provider_with_auth()
 * when cfg->provider is "openai" and openai_auth is set) and copies every
 * config field into the new agent.
 *
 * Return: caller-owned Agent, or NULL when provider creation or an
 * allocation fails (an error is logged). Release with agent_destroy().
 * Not thread-safe: the agent is mutable shared state and must not be used
 * by concurrent runs.
 */
Agent *agent_create(const AgentConfig *cfg);

/**
 * agent_destroy - release an agent and everything it owns
 * @agent: agent to release, or NULL (no-op).
 *
 * Destroys the provider, clears and frees all messages, frees the model,
 * provider name/token, system prompt, session id, context summary, effort,
 * and the circuit breaker.
 *
 * Return: void. Safe on NULL. Not thread-safe: must not run concurrently
 * with any other agent_* call on the same agent.
 */
void agent_destroy(Agent *agent);

/**
 * agent_run - run one full agent turn (buffered, non-streaming)
 * @agent: agent to run; must be non-NULL.
 * @user_input: user message text, borrowed for the duration of the call;
 *   a copy is appended to the agent's message list.
 *
 * Appends the user turn, then loops: LLM call, tool execution, until the
 * model produces a final answer or max_iterations is exhausted. The
 * session is saved after every message and a title is generated once the
 * run finishes (when a session manager is attached). Cancellation is
 * checked before each LLM call.
 *
 * Return: caller-owned LLMResponse (free with llm_response_free()), or
 * NULL on cancellation, circuit-breaker rejection, max iterations, or
 * allocation failure. Not thread-safe; mutates the agent's message
 * context, so concurrent runs on the same agent are undefined.
 */
LLMResponse *agent_run(Agent *agent, const char *user_input);

/**
 * agent_run_streaming - run one full agent turn, streaming output
 * @agent: agent to run; must be non-NULL.
 * @user_input: user message text, borrowed for the duration of the call;
 *   a copy is appended to the agent's message list.
 * @on_chunk: callback fired per content delta as the provider streams it,
 *   or NULL to discard chunks. Called synchronously from the run loop.
 * @userdata: opaque pointer forwarded to on_chunk unchanged.
 *
 * Same loop as agent_run() but each LLM call uses the provider's
 * streaming path, so chunks arrive while the response is still in flight.
 *
 * Return: caller-owned aggregated LLMResponse (free with
 * llm_response_free()), or NULL on cancellation, circuit-breaker
 * rejection, max iterations, or allocation failure. Not thread-safe;
 * mutates the agent's message context.
 */
LLMResponse *agent_run_streaming(Agent *agent, const char *user_input,
                                 void (*on_chunk)(const char *chunk, void *userdata),
                                 void *userdata);

/**
 * agent_run_streaming_context - run the streaming loop on the current context
 * @agent: agent to run; must be non-NULL.
 * @on_chunk: callback fired per content delta, or NULL to discard chunks.
 *   Called synchronously from the run loop.
 * @userdata: opaque pointer forwarded to on_chunk unchanged.
 *
 * Runs the same loop as agent_run_streaming() WITHOUT appending a user
 * turn. The caller must have already appended the final user/assistant
 * message — the fork message in the edit/regenerate flow — so it stays
 * the last context entry.
 *
 * Return: caller-owned aggregated LLMResponse (free with
 * llm_response_free()), or NULL on cancellation, circuit-breaker
 * rejection, max iterations, or allocation failure. Not thread-safe;
 * mutates the agent's message context.
 */
LLMResponse *agent_run_streaming_context(Agent *agent,
                                         void (*on_chunk)(const char *chunk, void *userdata),
                                         void *userdata);

/**
 * agent_set_session_manager - attach the session persistence layer
 * @agent: agent to configure; must be non-NULL.
 * @sm: session manager, borrowed; the agent does not own it, it must
 *   outlive the agent. NULL detaches persistence (sessions are then never
 *   saved).
 *
 * Return: void. Not thread-safe.
 */
void agent_set_session_manager(Agent *agent, SessionManager *sm);

/**
 * agent_set_approval_callback - register the tool-approval callback
 * @agent: agent to configure; must be non-NULL.
 * @cb: approval hook invoked before executing a tool that
 *   safety_needs_approval() flags, with (tool name, arguments, userdata);
 *   returning 0 denies the tool. NULL (or no callback set) makes every
 *   approval-required tool denied. Borrowed, must outlive the agent.
 * @userdata: opaque pointer forwarded to cb unchanged, borrowed.
 *
 * Return: void. Not thread-safe.
 */
void agent_set_approval_callback(Agent *agent,
                                 int (*cb)(const char *, const char *, void *),
                                 void *userdata);

/**
 * agent_cancel - request cancellation of the running agent turn
 * @agent: agent to cancel; must be non-NULL.
 *
 * Sets a volatile flag the run loops check before each LLM call; an
 * in-flight LLM call itself is not interrupted. Safe to call from another
 * thread while a run is in progress.
 *
 * Return: void.
 */
void agent_cancel(Agent *agent);

/**
 * agent_set_metrics - attach the metrics reporter
 * @agent: agent to configure; must be non-NULL.
 * @metrics: metrics instance, borrowed; must outlive the agent. NULL
 *   disables metrics recording.
 *
 * Return: void. Not thread-safe.
 */
void agent_set_metrics(Agent *agent, Metrics *metrics);

/**
 * agent_set_title_callback - register the title-update callback
 * @agent: agent to configure; must be non-NULL.
 * @cb: fired with (session id, title, userdata) once a title has been
 *   generated for the session, or NULL to disable. Borrowed, must outlive
 *   the agent.
 * @userdata: opaque pointer forwarded to cb unchanged, borrowed.
 *
 * Return: void. Not thread-safe.
 */
void agent_set_title_callback(Agent *agent, title_callback cb, void *userdata);

/**
 * agent_set_tool_start_callback - register the tool-start callback
 * @agent: agent to configure; must be non-NULL.
 * @cb: fired with (tool name, arguments JSON, userdata) before each tool
 *   executes, or NULL to disable. Borrowed, must outlive the agent.
 * @userdata: opaque pointer forwarded to cb unchanged, borrowed.
 *
 * Return: void. Not thread-safe.
 */
void agent_set_tool_start_callback(Agent *agent, tool_start_callback cb, void *userdata);

/**
 * agent_set_tool_end_callback - register the tool-end callback
 * @agent: agent to configure; must be non-NULL.
 * @cb: fired with (tool name, tool call id, result content, result error,
 *   userdata) after each tool executes, or NULL to disable. Borrowed,
 *   must outlive the agent.
 * @userdata: opaque pointer forwarded to cb unchanged, borrowed.
 *
 * Return: void. Not thread-safe.
 */
void agent_set_tool_end_callback(Agent *agent, tool_end_callback cb, void *userdata);

/**
 * agent_set_ask_user_callback - register the ask-user callback
 * @agent: agent to configure; must be non-NULL.
 * @cb: invoked when the ask_user tool runs and needs interactive input,
 *   with (question, userdata), or NULL to fall back to stdin input.
 *   Borrowed, must outlive the agent.
 * @userdata: opaque pointer forwarded to cb unchanged, borrowed.
 *
 * The callback returns the user's answer as a heap-allocated string that
 * the ask_user tool frees, or NULL to cancel the question.
 *
 * Return: void. Not thread-safe.
 */
void agent_set_ask_user_callback(Agent *agent, ask_user_callback cb, void *userdata);

/**
 * agent_set_safety - attach the safety configuration
 * @agent: agent to configure; must be non-NULL.
 * @safety: safety config, borrowed; must outlive the agent. NULL
 *   disables approval gating (all tools run without prompting).
 *
 * Return: void. Not thread-safe.
 */
void agent_set_safety(Agent *agent, SafetyConfig *safety);

/**
 * agent_set_callback_manager - attach the callback manager
 * @agent: agent to configure; must be non-NULL.
 * @mgr: callback manager, borrowed; must outlive the agent. NULL
 *   disables run/LLM/tool lifecycle callbacks (the agent's own callback
 *   fields still fire).
 *
 * Return: void. Not thread-safe.
 */
void agent_set_callback_manager(Agent *agent, CallbackManager *mgr);

/**
 * agent_set_model - switch the model name used for LLM calls
 * @agent: agent to configure; must be non-NULL.
 * @model: model name; the agent keeps its own copy. NULL is ignored and
 *   the current model is kept.
 *
 * Return: void. Not thread-safe.
 */
void agent_set_model(Agent *agent, const char *model);

/**
 * agent_set_provider - swap the live LLM provider
 * @agent: agent to configure; must be non-NULL.
 * @provider: canonical provider factory name, e.g. "ollama"/"openai".
 * @base_url: endpoint, or NULL for the provider default. Borrowed.
 * @api_token: API token, or NULL/empty for none. Borrowed.
 * @num_ctx: context window size passed to get_provider().
 * @keep_alive_secs: model keep-alive seconds passed to get_provider().
 * @effort: reasoning-effort hint passed to get_provider() the same way
 *   as agent_create()'s (NULL = provider default). Borrowed.
 *
 * Builds the replacement provider first so a failure leaves the old
 * provider untouched and still owned by the agent. A no-op when the
 * provider name and effort are unchanged — the model is switched
 * separately via agent_set_model().
 *
 * Return: 0 on success, -1 on invalid arguments, provider creation
 * failure, or allocation failure; in every failure case the old provider
 * is kept. The caller keeps no ownership of anything. Not thread-safe.
 */
int agent_set_provider(Agent *agent, const char *provider, const char *base_url,
                       const char *api_token, int num_ctx, int keep_alive_secs,
                       const char *effort);

#endif
