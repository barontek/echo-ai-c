/*
 * read_file.c - file reading tool: reads a file's contents for the model,
 * bounded by the safety policy's size and path rules. Depends on: tool.h,
 * safety, string_utils, logging.
 */

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

static ToolResult *read_file_execute(Tool *self, const char *args_json)
{
    FileCtx *ctx = self->ctx;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *path_json = cJSON_GetObjectItem(args, "file_path");
    if (!path_json || !cJSON_IsString(path_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'file_path' argument", "validation_error");
    }

    char *path = str_dup(cJSON_GetStringValue(path_json));
    cJSON_Delete(args);

    if (!safety_check_path(ctx->safety, path))
    {
        free(path);
        return tool_result_error("path rejected by safety policy", "policy_denied");
    }

    char *resolved = safety_resolve_path(ctx->safety, path);
    free(path);
    if (!resolved) return tool_result_error("path resolution failed", "execution_error");

    FILE *fp = fopen(resolved, "r");
    if (!fp)
    {
        free(resolved);
        return tool_result_error("file not found", "file_not_found");
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size > (long)ctx->safety->max_file_size)
    {
        fclose(fp);
        free(resolved);
        return tool_result_error("file exceeds max size", "policy_denied");
    }
    fseek(fp, 0, SEEK_SET);

    char *content = malloc((size_t)size + 1);
    if (!content) { fclose(fp); free(resolved); return NULL; }

    size_t read_size = fread(content, 1, (size_t)size, fp);
    content[read_size] = '\0';
    fclose(fp);
    free(resolved);

    ToolResult *tr = tool_result_create(content);
    free(content);
    return tr;
}

static void read_file_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_read_file_create - construct the read_file tool
 * @safety: borrowed SafetyConfig consulted on every execution; not owned
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
Tool *tool_read_file_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    FileCtx *ctx = calloc(1, sizeof(FileCtx));
    if (!ctx) { free(t); return NULL; }
    ctx->safety = safety;

    t->name = str_dup("read_file");
    t->description = str_dup("Read contents of a file");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"file_path\":{\"type\":\"string\",\"description\":\"Path to the file\"}"
        "},\"required\":[\"file_path\"]}"
    );
    t->execute = read_file_execute;
    t->destroy = read_file_destroy;
    t->ctx = ctx;
    return t;
}
