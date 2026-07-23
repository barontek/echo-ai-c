#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
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

    const char *path = cJSON_GetStringValue(path_json);
    const char *content = cJSON_GetStringValue(content_json);
    cJSON_Delete(args);

    if (!safety_check_path(ctx->safety, path))
        return tool_result_error("path rejected by safety policy", "policy_denied");

    if (strlen(content) > ctx->safety->max_file_size)
        return tool_result_error("content exceeds max file size", "policy_denied");

    char *resolved = safety_resolve_path(ctx->safety, path);
    if (!resolved) return tool_result_error("path resolution failed", "execution_error");

    FILE *fp = fopen(resolved, "w");
    if (!fp)
    {
        free(resolved);
        return tool_result_error("cannot write to path", "permission_denied");
    }

    fwrite(content, 1, strlen(content), fp);
    fclose(fp);
    free(resolved);

    char *result = NULL;
    if (asprintf(&result, "Written %zu bytes to %s", strlen(content), path) < 0)
        result = NULL;
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
