/*
 * openai_request.h - Codex request-body builder contracts.
 * Depends on: openai_internal.h, message.h.
 */

#ifndef ECHO_OPENAI_REQUEST_H
#define ECHO_OPENAI_REQUEST_H

#include <stddef.h>

#include "openai_internal.h"

/**
 * parse_bounded_json - parse text up to a byte limit
 * @text: NUL-terminated JSON text.
 * @limit: maximum accepted length in bytes.
 * @json_out: receives a caller-owned cJSON tree (cJSON_Delete()).
 *
 * Return: 0 on success, -1 on NULL args, empty/oversized text, or
 * parse failure with *json_out NULL.
 */
int parse_bounded_json(const char *text, size_t limit, cJSON **json_out);

/**
 * valid_nonempty_string - check a cJSON value is a non-empty string
 * @item: cJSON value to test; NULL fails.
 *
 * Return: 1 when item is a string with a non-empty value, 0 otherwise.
 */
int valid_nonempty_string(const cJSON *item);

/**
 * build_request_body - serialize a Codex chat request body
 * @messages: array of messages; may be NULL only when count is 0.
 * @count: number of messages (>= 0).
 * @model: model name; must be non-empty.
 * @temperature: sampling temperature; must be finite and in [0, 2].
 * @stream: 1 for a streaming request, 0 otherwise.
 * @tools_json: pre-serialized OpenAI-style tools array JSON, or NULL/empty.
 * @json_schema: pre-serialized JSON schema, or NULL/empty for none.
 * @effort: reasoning-effort hint, or NULL/empty for the API default.
 *
 * Return: caller-owned JSON string (free with free()), or NULL on
 * invalid arguments, malformed messages/tools, or allocation failure.
 */
char *build_request_body(Message *messages, int count, const char *model,
                         double temperature, int stream,
                         const char *tools_json, const char *json_schema,
                         const char *effort);

#endif /* ECHO_OPENAI_REQUEST_H */
