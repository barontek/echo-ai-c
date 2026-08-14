/*
 * tool_args.h - compact one-line rendering of tool-call argument JSON.
 * Depends on: cJSON, stdlib.
 */

#ifndef ECHO_TOOL_ARGS_H
#define ECHO_TOOL_ARGS_H

#include <stddef.h>

/**
 * tool_args_compact - flatten a tool-call arguments JSON object to one line
 * @json: NUL-terminated JSON string as the model produced it (usually an
 *   object like {"command": "ls -la"}); may be NULL or not JSON.
 * @max_len: hard cap in bytes for the result, excluding the trailing NUL
 *   and the "…" marker; must be >= 1. 0 is treated as 1.
 *
 * Object entries become "key=value" joined by ", " with string values
 * shown bare and non-string values as compact JSON. The result is
 * truncated to @max_len at a UTF-8 codepoint boundary with "…" appended,
 * so the caller can paste it into a one-line header without further
 * bounds math. Non-object or unparseable input degrades to the raw
 * input, truncated the same way. An empty object yields "".
 *
 * Return: freshly malloc'd NUL-terminated string owned by the caller
 *   (free with free()), or NULL on allocation failure. Thread-safety:
 *   no shared state; safe to call concurrently.
 */
char *tool_args_compact(const char *json, size_t max_len);

#endif /* ECHO_TOOL_ARGS_H */
