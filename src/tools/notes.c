#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
    char *notes_dir;
} NotesCtx;

static const char *notes_dir_path(NotesCtx *ctx)
{
    if (ctx->notes_dir) return ctx->notes_dir;

    const char *home = getenv("HOME");
    if (!home) return "/tmp/echo-notes";

    char *path = NULL;
    if (asprintf(&path, "%s/.config/echo-ai/notes", home) >= 0)
    {
        ctx->notes_dir = path;
        mkdir(path, 0755);
    }
    return ctx->notes_dir ? ctx->notes_dir : "/tmp/echo-notes";
}

static ToolResult *notes_execute(Tool *self, const char *args_json)
{
    NotesCtx *ctx = self->ctx;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *action_json = cJSON_GetObjectItem(args, "action");
    if (!action_json || !cJSON_IsString(action_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'action' argument (list, read, write, delete)",
                                 "validation_error");
    }

    const char *action = cJSON_GetStringValue(action_json);
    cJSON *name_json = cJSON_GetObjectItem(args, "name");

    const char *ndir = notes_dir_path(ctx);

    if (strcmp(action, "list") == 0)
    {
        cJSON_Delete(args);
        DIR *dir = opendir(ndir);
        if (!dir) return tool_result_create("(no notes)");

        cJSON *arr = cJSON_CreateArray();
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            if (str_ends_with(entry->d_name, ".md"))
            {
                char *dot = strrchr(entry->d_name, '.');
                if (dot) *dot = '\0';
                cJSON_AddItemToArray(arr, cJSON_CreateString(entry->d_name));
                if (dot) *dot = '.';
            }
        }
        closedir(dir);

        char *result = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        ToolResult *tr = tool_result_create(result ? result : "(no notes)");
        free(result);
        return tr;
    }

    if (!name_json || !cJSON_IsString(name_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'name' argument", "validation_error");
    }

    const char *name = cJSON_GetStringValue(name_json);

    if (strcmp(action, "read") == 0)
    {
        cJSON_Delete(args);
        char *fpath = NULL;
        if (asprintf(&fpath, "%s/%s.md", ndir, name) < 0)
            return tool_result_error("oom", "execution_error");

        struct stat st;
        if (stat(fpath, &st) != 0) { free(fpath); return tool_result_error("note not found", "file_not_found"); }

        FILE *f = fopen(fpath, "rb");
        if (!f) { free(fpath); return tool_result_error("cannot read note", "execution_error"); }

        if (st.st_size > (long)ctx->safety->max_file_size)
        { fclose(f); free(fpath); return tool_result_error("note too large", "policy_denied"); }

        char *content = malloc((size_t)st.st_size + 1);
        if (!content) { fclose(f); free(fpath); return tool_result_error("oom", "execution_error"); }

        size_t read = fread(content, 1, (size_t)st.st_size, f);
        fclose(f);
        content[read] = '\0';
        free(fpath);

        ToolResult *tr = tool_result_create(content);
        free(content);
        return tr;
    }
    else if (strcmp(action, "write") == 0)
    {
        cJSON *content_json = cJSON_GetObjectItem(args, "content");
        if (!content_json || !cJSON_IsString(content_json))
        {
            cJSON_Delete(args);
            return tool_result_error("missing 'content' for write action", "validation_error");
        }
        const char *content = cJSON_GetStringValue(content_json);
        cJSON_Delete(args);

        char *fpath = NULL;
        if (asprintf(&fpath, "%s/%s.md", ndir, name) < 0)
            return tool_result_error("oom", "execution_error");

        FILE *f = fopen(fpath, "w");
        if (!f) { free(fpath); return tool_result_error("cannot write note", "execution_error"); }

        fwrite(content, 1, strlen(content), f);
        fclose(f);
        free(fpath);

        return tool_result_create("Note written successfully.");
    }
    else if (strcmp(action, "delete") == 0)
    {
        cJSON_Delete(args);
        char *fpath = NULL;
        if (asprintf(&fpath, "%s/%s.md", ndir, name) < 0)
            return tool_result_error("oom", "execution_error");

        int rc = unlink(fpath);
        free(fpath);

        if (rc != 0) return tool_result_error("note not found or cannot delete", "file_not_found");
        return tool_result_create("Note deleted.");
    }

    cJSON_Delete(args);
    return tool_result_error("unknown action (use list, read, write, delete)", "validation_error");
}

static void notes_destroy(Tool *self)
{
    if (!self) return;
    NotesCtx *ctx = self->ctx;
    free(ctx->notes_dir);
    free(ctx);
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self);
}

Tool *tool_notes_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    NotesCtx *ctx = calloc(1, sizeof(NotesCtx));
    if (!ctx) { free(t); return NULL; }
    ctx->safety = safety;

    t->name = str_dup("notes");
    t->description = str_dup("Manage personal notes (list, read, write, delete markdown notes)");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"read\",\"write\",\"delete\"],\"description\":\"Action to perform\"},"
        "\"name\":{\"type\":\"string\",\"description\":\"Note name (without .md extension)\"},"
        "\"content\":{\"type\":\"string\",\"description\":\"Note content (for write action)\"}"
        "},\"required\":[\"action\"]}"
    );
    t->execute = notes_execute;
    t->destroy = notes_destroy;
    t->ctx = ctx;
    return t;
}
