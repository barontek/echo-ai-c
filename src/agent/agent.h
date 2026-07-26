#ifndef ECHO_AGENT_H
#define ECHO_AGENT_H

#include "message.h"
#include "../llm/provider.h"
#include "../session/session_manager.h"
#include "../safety/safety.h"
#include "../utils/circuit_breaker.h"
#include "../utils/callbacks.h"
#include "../utils/metrics.h"

typedef struct {
    const char *provider;
    const char *model;
    const char *base_url;
    const char *system_prompt;
    double temperature;
    int timeout;
    int max_iterations;
    int max_context_messages;
    int max_context_chars;
    int num_ctx;
    int keep_alive_secs;
    int parallel_tool_exec;
} AgentConfig;

typedef void (*title_callback)(const char *session_id, const char *title, void *userdata);
typedef void (*tool_start_callback)(const char *tool_name, const char *arguments, void *userdata);
typedef void (*tool_end_callback)(const char *tool_name, const char *tool_call_id,
                                  const char *result_content, const char *result_error,
                                  void *userdata);

typedef struct {
    LLMProvider *provider;
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
    int parallel_tool_exec;
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
    SafetyConfig *safety;
} Agent;

Agent *agent_create(const AgentConfig *cfg);
void agent_destroy(Agent *agent);
LLMResponse *agent_run(Agent *agent, const char *user_input);
LLMResponse *agent_run_streaming(Agent *agent, const char *user_input,
                                 void (*on_chunk)(const char *chunk, void *userdata),
                                 void *userdata);
void agent_set_session_manager(Agent *agent, SessionManager *sm);
void agent_set_approval_callback(Agent *agent,
                                 int (*cb)(const char *, const char *, void *),
                                 void *userdata);
void agent_cancel(Agent *agent);
void agent_set_metrics(Agent *agent, Metrics *metrics);
void agent_set_title_callback(Agent *agent, title_callback cb, void *userdata);
void agent_set_tool_start_callback(Agent *agent, tool_start_callback cb, void *userdata);
void agent_set_tool_end_callback(Agent *agent, tool_end_callback cb, void *userdata);
void agent_set_safety(Agent *agent, SafetyConfig *safety);
void agent_set_callback_manager(Agent *agent, CallbackManager *mgr);

#endif
