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
} Message;

typedef struct {
    char *content;
    char *thinking;
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

ToolCall *tool_call_create(const char *id, const char *name, const char *arguments);
void tool_call_free(ToolCall *call);

LLMResponse *llm_response_create(void);
void llm_response_free(LLMResponse *resp);

cJSON *messages_to_json_array(Message *msgs, int count);
char *llm_messages_format(Message *msgs, int count,
                          const char *system_prompt,
                          const char *system_context);

#endif
