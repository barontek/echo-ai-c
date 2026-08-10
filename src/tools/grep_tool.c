/*
 * grep_tool.c - content search tool: recursively searches a directory for
 * lines containing a pattern, skipping symlinks, binary-looking files and
 * anything outside the workspace. Depends on: tool.h, safety, string_utils.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"

typedef struct {
    SafetyConfig *safety;
} GrepCtx;

static void search_file(const char *path, const char *pattern,
                        char *buffer, size_t cap, size_t *pos)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[4096];
    int line_num = 0;

    while (fgets(line, sizeof(line), fp))
    {
        line_num++;
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        if (strstr(line, pattern) != NULL)
        {
            *pos += snprintf(buffer + *pos, cap - *pos, "%s:%d:%s\n",
                             path, line_num, line);
            if (*pos >= cap - 1) break;
        }
    }

    fclose(fp);
}

static void search_dir(const char *dir_path, const char *pattern,
                        char *buffer, size_t cap, size_t *pos, int max_files,
                        const SafetyConfig *safety)
{
    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *entry;
    int files_scanned = 0;

    while ((entry = readdir(d)) != NULL)
    {
        if (files_scanned >= max_files) break;
        if (entry->d_name[0] == '.') continue;

        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (lstat(full_path, &st) != 0 || S_ISLNK(st.st_mode) ||
            !safety_path_is_within_workspace(safety, full_path)) continue;

        if (S_ISDIR(st.st_mode))
        {
            search_dir(full_path, pattern, buffer, cap, pos,
                       max_files - files_scanned, safety);
        }
        else if (S_ISREG(st.st_mode))
        {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext)
            {
                static const char *binary_exts[] = {".o", ".so", ".a", ".dylib",
                    ".exe", ".bin", ".jpg", ".png", ".gif", ".zip", ".tar", ".gz", NULL};
                int skip = 0;
                for (int i = 0; binary_exts[i]; i++)
                {
                    if (strcmp(ext, binary_exts[i]) == 0) { skip = 1; break; }
                }
                if (skip) continue;
            }

            search_file(full_path, pattern, buffer, cap, pos);
            files_scanned++;
        }
    }

    closedir(d);
}

static ToolResult *grep_execute(Tool *self, const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *pattern_json = cJSON_GetObjectItem(args, "pattern");
    cJSON *path_json = cJSON_GetObjectItem(args, "path");

    if (!pattern_json || !cJSON_IsString(pattern_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'pattern' argument", "validation_error");
    }

    char *pattern = str_dup(cJSON_GetStringValue(pattern_json));
    int has_path = path_json && cJSON_IsString(path_json);
    char *search_path = has_path ? str_dup(cJSON_GetStringValue(path_json)) : str_dup(".");
    cJSON_Delete(args);

    GrepCtx *gctx = (GrepCtx *)self->ctx;
    if (gctx && gctx->safety && !safety_check_path(gctx->safety, search_path))
    {
        free(pattern);
        free(search_path);
        return tool_result_error("path rejected by safety check", "validation_error");
    }

    char *resolved = safety_resolve_path_alloc(gctx->safety, search_path);
    free(search_path);
    if (!resolved)
    {
        free(pattern);
        return tool_result_error("path resolution failed", "validation_error");
    }

    char buffer[32768] = {0};
    size_t pos = 0;

    search_dir(resolved, pattern, buffer, sizeof(buffer), &pos, 100,
               gctx->safety);
    free(pattern);
    free(resolved);

    ToolResult *tr = tool_result_create(buffer[0] ? buffer : "(no matches)");
    return tr;
}

static void grep_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_grep_create - construct the grep tool
 * @safety: borrowed SafetyConfig consulted on every execution; not owned
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
Tool *tool_grep_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    GrepCtx *ctx = calloc(1, sizeof(GrepCtx));
    if (!ctx) { free(t); return NULL; }
    ctx->safety = safety;

    t->name = str_dup("grep");
    t->description = str_dup("Search file contents for a pattern");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"pattern\":{\"type\":\"string\",\"description\":\"Text pattern to search for\"},"
        "\"path\":{\"type\":\"string\",\"description\":\"Directory to search (default: .)\"}"
        "},\"required\":[\"pattern\"]}"
    );
    t->execute = grep_execute;
    t->destroy = grep_destroy;
    t->ctx = ctx;
    return t;
}
