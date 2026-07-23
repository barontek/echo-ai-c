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
    const char *dir_path = path_json && cJSON_IsString(path_json)
        ? cJSON_GetStringValue(path_json) : ".";
    cJSON_Delete(args);

    char *resolved = safety_resolve_path(ctx->safety, dir_path);
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
