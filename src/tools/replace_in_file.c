/*
 * replace_in_file.c - text replacement tool: finds old_string in a file
 * and replaces the first occurrence with new_string, in place.
 * Depends on: tool.h, safety, string_utils, logging.
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
} ReplaceCtx;

static ToolResult *replace_in_file_execute(Tool *self, const char *args_json)
{
    ReplaceCtx *ctx = self->ctx;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *path_json = cJSON_GetObjectItem(args, "path");
    cJSON *old_json = cJSON_GetObjectItem(args, "old_string");
    cJSON *new_json = cJSON_GetObjectItem(args, "new_string");

    if (!path_json || !cJSON_IsString(path_json) ||
        !old_json || !cJSON_IsString(old_json) ||
        !new_json || !cJSON_IsString(new_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing required arguments: path, old_string, new_string",
                                 "validation_error");
    }

    char *path = str_dup(cJSON_GetStringValue(path_json));
    char *old_str = str_dup(cJSON_GetStringValue(old_json));
    char *new_str = str_dup(cJSON_GetStringValue(new_json));
    cJSON_Delete(args);

    if (!path || !old_str || !new_str)
    {
        free(path); free(old_str); free(new_str);
        return tool_result_error("oom", "execution_error");
    }

    if (!safety_check_path(ctx->safety, path))
    {
        free(path); free(old_str); free(new_str);
        return tool_result_error("path rejected by safety check", "policy_denied");
    }

    char *resolved = safety_resolve_path_alloc(ctx->safety, path);
    if (!resolved)
    {
        free(path); free(old_str); free(new_str);
        return tool_result_error("path resolution failed", "policy_denied");
    }

    FILE *f = fopen(resolved, "rb");
    if (!f)
    {
        free(resolved);
        free(path); free(old_str); free(new_str);
        return tool_result_error("file not found", "file_not_found");
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    if (fsize <= 0 || (size_t)fsize > ctx->safety->max_file_size)
    {
        fclose(f);
        free(resolved);
        free(path); free(old_str); free(new_str);
        return tool_result_error("file empty or too large", "policy_denied");
    }

    char *content = malloc((size_t)fsize + 1);
    if (!content) { fclose(f); free(resolved); free(path); free(old_str); free(new_str);
        return tool_result_error("oom", "execution_error"); }

    size_t read = fread(content, 1, (size_t)fsize, f);
    fclose(f);
    content[read] = '\0';

    char *pos = strstr(content, old_str);
    if (!pos)
    {
        free(content);
        free(resolved);
        free(path); free(old_str); free(new_str);
        return tool_result_create("No match found for old_string in file.");
    }

    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    size_t prefix_len = (size_t)(pos - content);
    size_t suffix_len = read - prefix_len - old_len;

    char *new_content = malloc(prefix_len + new_len + suffix_len + 1);
    if (!new_content) { free(content); free(resolved); free(path); free(old_str); free(new_str);
        return tool_result_error("oom", "execution_error"); }

    memcpy(new_content, content, prefix_len);
    memcpy(new_content + prefix_len, new_str, new_len);
    memcpy(new_content + prefix_len + new_len, pos + old_len, suffix_len + 1);

    free(content);

    f = fopen(resolved, "wb");
    if (!f) { free(new_content); free(resolved); free(path); free(old_str); free(new_str);
        return tool_result_error("cannot write file", "execution_error"); }

    size_t written = fwrite(new_content, 1, prefix_len + new_len + suffix_len, f);
    fclose(f);

    free(new_content);
    free(resolved);
    free(path);
    free(old_str);
    free(new_str);

    char *result = NULL;
    if (asprintf(&result, "Replaced 1 occurrence (%zu bytes written).", written) < 0)
        result = str_dup("Replaced 1 occurrence.");

    ToolResult *tr = tool_result_create(result);
    free(result);
    return tr;
}

static void replace_in_file_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_replace_in_file_create - construct the replace_in_file tool
 * @safety: borrowed SafetyConfig consulted on every execution; not owned
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
Tool *tool_replace_in_file_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    ReplaceCtx *ctx = calloc(1, sizeof(ReplaceCtx));
    if (!ctx) { free(t); return NULL; }
    ctx->safety = safety;

    t->name = str_dup("replace_in_file");
    t->description = str_dup("Replace text in a file. Finds old_string and replaces it with new_string.");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"File path\"},"
        "\"old_string\":{\"type\":\"string\",\"description\":\"Text to find\"},"
        "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}"
        "},\"required\":[\"path\",\"old_string\",\"new_string\"]}"
    );
    t->execute = replace_in_file_execute;
    t->destroy = replace_in_file_destroy;
    t->ctx = ctx;
    return t;
}
