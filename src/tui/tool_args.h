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

/**
 * tool_args_compact_named - compact tool-call args, with a per-tool
 *   rendering special case for the edit tool
 * @tool_name: tool name as the model called it (e.g. "edit"); NULL or
 *   any unrecognized name falls back to tool_args_compact.
 * @json: same input as tool_args_compact.
 * @max_len: same budget as tool_args_compact.
 *
 * The edit tool's header shows only the file name (basename of path),
 * matching opencode's edit header — the change itself is displayed in
 * the tool result diff, not in the one-line summary. A call that fails
 * to parse or lacks a path degrades to the generic rendering. Every
 * other tool renders identically to tool_args_compact.
 *
 * Return: freshly malloc'd NUL-terminated string owned by the caller
 *   (free with free()), or NULL on allocation failure. Thread-safety:
 *   no shared state; safe to call concurrently.
 */
char *tool_args_compact_named(const char *tool_name, const char *json,
                              size_t max_len);

/**
 * tool_args_label - human-readable tool label for chat headers
 * @name: raw tool name as the model called it (e.g. "read_file"); may be
 *   NULL.
 *
 * Maps known tool names to friendly labels ("Bash", "Read", "Web
 * Search"); unknown names pass through unchanged. The raw name stays the
 * block title so tool_end can match pending blocks by name while the
 * header displays this label.
 *
 * Return: static string (never freed); never NULL.
 */
const char *tool_args_label(const char *name);

#endif /* ECHO_TOOL_ARGS_H */
