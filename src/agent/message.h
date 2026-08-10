/*
 * message.h - message/tool-call data structures with ownership helpers,
 * LLM response types, and JSON serialization for chat payloads.
 * Depends on: cJSON.
 */

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
    /* Stable message identity. id is NULL for legacy messages that were
     * created before id-minting existed; fork points get ids minted at
     * fork time. parent_id is the id of the preceding message in this
     * message's chain (NULL for the chain root or when the parent has no
     * id). fork_group_id is the logical node identity shared by every
     * chain's copy of the same fork point; NULL for non-fork messages. */
    char *id;
    char *parent_id;
    char *fork_group_id;
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

/**
 * message_create - allocate a message with role/content and a timestamp
 * @role: role string, e.g. "user"/"assistant"/"tool"/"system". Borrowed;
 *   the message keeps its own copy. NULL makes the call fail.
 * @content: content string, borrowed; copied (NULL is stored as "").
 *
 * Return: caller-owned Message (free with message_free()), or NULL on
 * allocation failure. Thread-safe; no shared state.
 */
Message *message_create(const char *role, const char *content);

/**
 * message_set_tool_calls - replace a message's tool calls, taking ownership
 * @msg: message to modify; must be non-NULL.
 * @calls: array of count tool calls. Ownership transfers to msg — it is
 *   freed (with the old array, if any) when msg is cleared or freed.
 * @count: number of entries in calls.
 *
 * Frees any tool calls the message already owns before adopting the new
 * array. Borrowed pointers inside calls are str_dup'ed on message_copy()
 * only — after this call, do not free calls yourself.
 *
 * Return: void.
 */
void message_set_tool_calls(Message *msg, ToolCall *calls, int count);

/**
 * message_copy - deep-copy src into dst (dst is overwritten)
 * @dst: destination message; its previous contents are discarded without
 *   being freed — pass a zeroed or already-cleared message. On failure dst
 *   is zeroed and safe to free.
 * @src: source message, borrowed and left untouched.
 *
 * Copies every string field, the timestamp, and the tool_calls array (if
 * any). NULL source or destination returns -1 without touching dst.
 *
 * Return: 0 on success (dst owns its copies, free via message_free_all()
 * style teardown or message_clear()), -1 on NULL arguments or allocation
 * failure. Thread-safe; no shared state.
 */
int message_copy(Message *dst, const Message *src);

/**
 * message_free - free a message and everything it owns
 * @msg: message to release, or NULL (no-op).
 *
 * Frees every owned string field, the tool_calls array, and msg itself.
 *
 * Return: void. Thread-safe; no shared state.
 */
void message_free(Message *msg);

/**
 * message_free_all - clear and free an array of messages
 * @msgs: array of count messages to release (each entry is cleared, the
 *   array itself is freed).
 * @count: number of entries in msgs.
 *
 * Return: void.
 */
void message_free_all(Message *msgs, int count);

/**
 * message_clear - free a message's contents without freeing the message
 * @msg: message to clear, or NULL (no-op).
 *
 * Frees all owned fields (role, content, id, tool_call_id, tool_name,
 * error_category, thinking, phase, provider_state, and every owned entry
 * of tool_calls) and resets the pointers/counts to NULL/0. Does NOT free
 * msg itself — the caller still owns the surrounding array (or single
 * Message allocation). After return, *msg is safe to discard or
 * re-initialize. Use this for every per-message teardown path so that
 * adding a field to Message only requires updating one place.
 *
 * Return: void.
 */
void message_clear(Message *msg);

/**
 * tool_call_create - allocate a tool call with id/name/arguments
 * @id: tool call id, borrowed; copied. NULL makes the call fail.
 * @name: tool name, borrowed; copied. NULL makes the call fail.
 * @arguments: arguments JSON string, borrowed; copied. NULL makes the
 *   call fail.
 *
 * Return: caller-owned ToolCall (free with tool_call_free()), or NULL on
 * allocation failure. Thread-safe; no shared state.
 */
ToolCall *tool_call_create(const char *id, const char *name, const char *arguments);

/**
 * tool_call_free - free a tool call's string fields, not the struct
 * @call: tool call whose owned strings to free, or NULL (no-op).
 *
 * The ToolCall struct itself is NOT freed — it is either heap-allocated by
 * the caller (who frees it) or inline in a Message/LLMResponse array that
 * owns it.
 *
 * Return: void.
 */
void tool_call_free(ToolCall *call);

/**
 * llm_response_create - allocate an empty LLM response
 *
 * Return: caller-owned zeroed LLMResponse (free with llm_response_free()),
 * or NULL on allocation failure. Thread-safe; no shared state.
 */
LLMResponse *llm_response_create(void);

/**
 * llm_response_free - free an LLM response and everything it owns
 * @resp: response to release, or NULL (no-op).
 *
 * Frees content, thinking, phase, provider_state, the tool_calls array,
 * and resp itself.
 *
 * Return: void.
 */
void llm_response_free(LLMResponse *resp);

/**
 * messages_to_json_array - serialize messages to a cJSON array
 * @msgs: messages to serialize, borrowed and not modified.
 * @count: number of messages in msgs.
 *
 * Each message becomes an object with role/content plus, when present:
 * thinking, phase, provider_state, tool_name, error_category, id,
 * parent_id, fork_group_id, timestamp, tool_calls (with persisted result
 * fields), and tool_call_id. NULL fields are omitted so legacy blobs stay
 * shape-equivalent.
 *
 * Return: caller-owned cJSON array (free with cJSON_Delete()), or NULL on
 * allocation failure. Thread-safe; no shared state.
 */
cJSON *messages_to_json_array(Message *msgs, int count);

/**
 * llm_messages_format - serialize messages to a JSON string with an
 * optional system prompt
 * @msgs: messages to serialize, borrowed and not modified.
 * @count: number of messages in msgs.
 * @system_prompt: optional system prompt prepended as the first message;
 *   NULL omits it.
 * @system_context: optional context appended to the system prompt with a
 *   blank line ("\n\n"); only used when system_prompt is non-NULL.
 *
 * Return: caller-owned JSON string (free with free()), or NULL on
 * allocation failure. Thread-safe; no shared state.
 */
char *llm_messages_format(Message *msgs, int count,
                          const char *system_prompt,
                          const char *system_context);

#endif
