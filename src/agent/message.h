#ifndef ECHO_MESSAGE_H
#define ECHO_MESSAGE_H

#include <cjson/cJSON.h>

typedef struct {
    char *id;
    char *name;
    char *arguments;
    char *result_content;
    char *result_error;
} ToolCall;

typedef struct {
    char *role;
    char *content;
    char *id;
    ToolCall *tool_calls;
    int tool_calls_count;
    char *tool_call_id;
    char *tool_name;
    char *error_category;
    double timestamp;
    char *thinking;
    char *phase;
    char *provider_state;
} Message;

typedef struct {
    char *content;
    char *thinking;
    char *phase;
    char *provider_state;
    ToolCall *tool_calls;
    int tool_calls_count;
} LLMResponse;

Message *message_create(const char *role, const char *content);
void message_set_tool_calls(Message *msg, ToolCall *calls, int count);

/* Deep-copies src into dst (dst is overwritten). On success dst owns its
 * copies and must be freed via message_free_all-style cleanup; src is left
 * untouched. On failure dst is zeroed and safe to free; returns -1. */
int message_copy(Message *dst, const Message *src);

void message_free(Message *msg);
void message_free_all(Message *msgs, int count);

/* C15: clear all owned fields of `msg` (role, content, id, tool_call_id,
 * tool_name, error_category, thinking, phase, provider_state, and every owned
 * entry of `tool_calls`)
 * AND reset the field pointers / counts to NULL / 0. Does NOT free `msg`
 * itself — the caller still owns the surrounding array (or single Message
 * allocation). Use this for every per-message teardown path so that adding a
 * field to Message only requires updating one place. After return, `*msg` is
 * safe to discard or re-initialize. */
void message_clear(Message *msg);

ToolCall *tool_call_create(const char *id, const char *name, const char *arguments);
void tool_call_free(ToolCall *call);

LLMResponse *llm_response_create(void);
void llm_response_free(LLMResponse *resp);

cJSON *messages_to_json_array(Message *msgs, int count);
char *llm_messages_format(Message *msgs, int count,
                          const char *system_prompt,
                          const char *system_context);

#endif
