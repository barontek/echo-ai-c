/*
 * list_dir.c - directory listing tool: returns one entry per line with a
 * trailing slash on directories, filtered through the safety policy's
 * path rules. Depends on: tool.h, safety, string_utils.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"

typedef struct {
    SafetyConfig *safety;
} DirCtx;

static ToolResult *list_dir_execute(Tool *self, const char *args_json)
{
    DirCtx *ctx = self->ctx;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *path_json = cJSON_GetObjectItem(args, "path");
    int has_path = path_json && cJSON_IsString(path_json);
    char *dir_path = has_path ? str_dup(cJSON_GetStringValue(path_json)) : str_dup(".");
    cJSON_Delete(args);

    char *resolved = safety_resolve_path_alloc(ctx->safety, dir_path);
    free(dir_path);
    if (!resolved) return tool_result_error("path resolution failed", "execution_error");

    DIR *d = opendir(resolved);
    if (!d)
    {
        free(resolved);
        return tool_result_error("cannot list directory", "file_not_found");
    }

    char buffer[16384] = {0};
    size_t pos = 0;
    struct dirent *entry;

    while ((entry = readdir(d)) != NULL)
    {
        if (entry->d_name[0] == '.') continue;
        int is_dir = entry->d_type == DT_DIR;
        pos += snprintf(buffer + pos, sizeof(buffer) - pos, "%s%s\n",
                        entry->d_name, is_dir ? "/" : "");
        if (pos >= sizeof(buffer) - 1) break;
    }

    closedir(d);
    free(resolved);

    ToolResult *tr = tool_result_create(buffer[0] ? buffer : "(empty directory)");
    return tr;
}

static void list_dir_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_list_dir_create - construct the list_dir tool
 * @safety: borrowed SafetyConfig consulted on every execution; not owned
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
Tool *tool_list_dir_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    DirCtx *ctx = calloc(1, sizeof(DirCtx));
    if (!ctx) { free(t); return NULL; }
    ctx->safety = safety;

    t->name = str_dup("list_dir");
    t->description = str_dup("List directory contents");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"Directory path (default: .)\"}"
        "}}"
    );
    t->execute = list_dir_execute;
    t->destroy = list_dir_destroy;
    t->ctx = ctx;
    return t;
}
