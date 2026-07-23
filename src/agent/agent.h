#ifndef ECHO_AGENT_H
#define ECHO_AGENT_H

#include "message.h"
#include "../llm/provider.h"
#include "../session/session_manager.h"
#include "../utils/circuit_breaker.h"

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
    int (*on_approval)(const char *tool_name, const char *arguments, void *userdata);
    void *approval_userdata;
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

#endif
