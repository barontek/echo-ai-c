/*
 * tool.h - Core Tool and ToolResult contracts shared by every built-in
 * tool module. Depends on: cJSON.
 */

#ifndef ECHO_TOOL_H
#define ECHO_TOOL_H

#include <cjson/cJSON.h>

/**
 * ToolResult - outcome of a single tool invocation.
 *
 * A success result has content set and error NULL; a failure result has
 * error (with error_category) set and content NULL. A field can be NULL
 * even on the "wrong" branch: construction failures leave the affected
 * field NULL instead of aborting, so callers must tolerate NULL in every
 * field. Heap-allocated; free with tool_result_free().
 */
typedef struct {
    char *content;          /* owned; output text when the call succeeded */
    char *error;            /* owned; human-readable message when it failed */
    char *error_category;   /* owned; machine category, e.g. validation_error */
} ToolResult;

typedef struct Tool Tool;

/**
 * Tool - a callable tool exposed to the LLM.
 *
 * The registry owns every Tool it holds and releases it via destroy().
 * execute() is not thread-safe: callers must not run it on the same tool
 * while another thread is still inside it. Tools keep per-instance state
 * in ctx.
 */
struct Tool {
    char *name;                  /* owned; registry key, e.g. "bash" */
    char *description;           /* owned; human-readable, sent to the LLM */
    char *parameters_schema;     /* owned; JSON schema string for the LLM */
    int enabled;                 /* 1 once registry_set_enabled() names it */
    ToolResult *(*execute)(Tool *self, const char *args_json); /* see below */
    void (*destroy)(Tool *self); /* frees self plus all owned fields and ctx */
    void *ctx;                   /* owned per-tool state; freed by destroy() */
};

/**
 * tool_result_create - build a success result carrying output text
 * @content: output text to copy; NULL or empty yields an empty result.
 *
 * Sets content only; error and error_category stay NULL. On allocation
 * failure of the struct itself, NULL is returned; if only the content
 * copy fails, a result with content == NULL is returned instead.
 *
 * Return: caller-owned ToolResult, freed with tool_result_free(). The
 * caller must tolerate a NULL content field. Thread-safe; no shared state.
 */
ToolResult *tool_result_create(const char *content);

/**
 * tool_result_error - build a failure result carrying an error message
 * @error: human-readable message to copy; NULL yields "unknown error".
 * @category: machine-readable category to copy; NULL yields
 *   "execution_error".
 *
 * Sets error and error_category only; content stays NULL. Same partial
 * failure behavior as tool_result_create: NULL is returned only when the
 * struct allocation fails; a failed string copy leaves that field NULL.
 *
 * Return: caller-owned ToolResult, freed with tool_result_free(). The
 * caller must tolerate NULL error fields. Thread-safe; no shared state.
 */
ToolResult *tool_result_error(const char *error, const char *category);

/**
 * tool_result_free - release a ToolResult and all its owned strings
 * @result: result to free, or NULL (no-op).
 *
 * Return: nothing. Not thread-safe against concurrent use of the same
 * result; freeing a result twice is a use-after-free.
 */
void tool_result_free(ToolResult *result);

#endif
