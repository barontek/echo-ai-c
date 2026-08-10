/*
 * write_file.c - file writing tool: writes model-provided content to a
 * file, snapshotting the pre-write state through the change tracker so
 * edits can be reverted. Depends on: tool.h, safety, change_tracker,
 * string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../change_tracker/change_tracker.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
    ChangeTracker *ct;
} FileCtx;

static ToolResult *write_file_execute(Tool *self, const char *args_json)
{
    FileCtx *ctx = self->ctx;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *path_json = cJSON_GetObjectItem(args, "file_path");
    cJSON *content_json = cJSON_GetObjectItem(args, "content");

    if (!path_json || !cJSON_IsString(path_json) || !content_json || !cJSON_IsString(content_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'file_path' or 'content' argument", "validation_error");
    }

    char *path = str_dup(cJSON_GetStringValue(path_json));
    char *content = str_dup(cJSON_GetStringValue(content_json));
    cJSON_Delete(args);

    if (!path || !content)
    {
        free(path);
        free(content);
        return tool_result_error("oom", "execution_error");
    }

    if (!safety_check_path(ctx->safety, path))
    {
        free(path);
        free(content);
        return tool_result_error("path rejected by safety policy", "policy_denied");
    }

    if (strlen(content) > ctx->safety->max_file_size)
    {
        free(path);
        free(content);
        return tool_result_error("content exceeds max file size", "policy_denied");
    }

    char *resolved = safety_resolve_path_alloc(ctx->safety, path);
    if (!resolved)
    {
        free(path);
        free(content);
        return tool_result_error("path resolution failed", "execution_error");
    }

    if (ctx->ct)
        ct_snapshot(ctx->ct, resolved);

    FILE *fp = fopen(resolved, "w");
    if (!fp)
    {
        free(resolved);
        free(path);
        free(content);
        return tool_result_error("cannot write to path", "permission_denied");
    }

    if (fwrite(content, 1, strlen(content), fp) != strlen(content) ||
        fclose(fp) != 0)
    {
        free(resolved);
        free(path);
        free(content);
        return tool_result_error("cannot write to file", "execution_error");
    }
    free(resolved);

    char *result = NULL;
    if (asprintf(&result, "Written %zu bytes to %s", strlen(content), path) < 0)
        result = NULL;
    free(path);
    free(content);
    ToolResult *tr = tool_result_create(result);
    free(result);
    return tr;
}

static void write_file_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_write_file_create - construct the write_file tool
 * @safety: borrowed SafetyConfig consulted on every execution; not owned
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
Tool *tool_write_file_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    FileCtx *ctx = calloc(1, sizeof(FileCtx));
    if (!ctx) { free(t); return NULL; }
    ctx->safety = safety;

    t->name = str_dup("write_file");
    t->description = str_dup("Write content to a file");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"file_path\":{\"type\":\"string\",\"description\":\"Path to the file\"},"
        "\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}"
        "},\"required\":[\"file_path\",\"content\"]}"
    );
    t->execute = write_file_execute;
    t->destroy = write_file_destroy;
    t->ctx = ctx;
    return t;
}

/**
 * tool_write_file_set_change_tracker - attach the change tracker used for
 * pre-write snapshots
 * @tool: tool returned by tool_write_file_create(); may be NULL
 * @ct: borrowed ChangeTracker; not owned, and may be NULL to disable
 * snapshots
 *
 * Return: void. Accepts NULL for either argument as a no-op.
 */
void tool_write_file_set_change_tracker(Tool *tool, ChangeTracker *ct)
{
    if (!tool || !tool->ctx) return;
    ((FileCtx *)tool->ctx)->ct = ct;
}
