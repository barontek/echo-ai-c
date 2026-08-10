/*
 * notes.c - personal notes tool: list/read/write/delete markdown notes
 * stored under the user's config dir, with strict name validation and
 * symlink-free file access. Depends on: tool.h, safety, string_utils,
 * logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>

#ifdef NOTES_TEST
#include <stdarg.h>
#endif

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

#ifdef NOTES_TEST
/* Test-only allocator fault injection: shared counter across str_dup and
 * asprintf so tests can fail the Nth allocation inside notes_execute.
 * Only the test target defines NOTES_TEST. */
static int notes_test_alloc_counter = 0;
static int notes_test_alloc_fail_at = -1;

void notes_test_set_alloc_fail(int nth_allocation)
{
    notes_test_alloc_counter = 0;
    notes_test_alloc_fail_at = nth_allocation;
}

static char *notes_test_strdup(const char *s)
{
    notes_test_alloc_counter++;
    if (notes_test_alloc_counter == notes_test_alloc_fail_at) return NULL;
    return str_dup(s);
}

static int notes_test_asprintf(char **strp, const char *fmt, ...)
{
    notes_test_alloc_counter++;
    if (notes_test_alloc_counter == notes_test_alloc_fail_at)
    {
        *strp = NULL;
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int rc = vasprintf(strp, fmt, ap);
    va_end(ap);
    return rc;
}

#define str_dup notes_test_strdup
#define asprintf notes_test_asprintf
#endif

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

static int valid_note_name(const char *name)
{
    if (!name || !name[0] || strlen(name) > 128 ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
    {
        if (!isalnum(*p) && *p != ' ' && *p != '_' && *p != '-' && *p != '.')
            return 0;
    }
    return 1;
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

    char *action = str_dup(cJSON_GetStringValue(action_json));
    if (!action)
    {
        cJSON_Delete(args);
        return tool_result_error("oom", "execution_error");
    }
    cJSON *name_json = cJSON_GetObjectItem(args, "name");

    const char *ndir = notes_dir_path(ctx);

    if (strcmp(action, "list") == 0)
    {
        free(action);
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

    char *name = NULL;
    char *note_content = NULL;

    if (!name_json || !cJSON_IsString(name_json))
    {
        cJSON_Delete(args);
        free(action);
        return tool_result_error("missing 'name' argument", "validation_error");
    }
    name = str_dup(cJSON_GetStringValue(name_json));
    if (!name)
    {
        cJSON_Delete(args);
        free(action);
        return tool_result_error("oom", "execution_error");
    }
    if (!valid_note_name(name))
    {
        cJSON_Delete(args);
        free(action);
        free(name);
        return tool_result_error("invalid note name", "validation_error");
    }

    cJSON *content_json = cJSON_GetObjectItem(args, "content");
    if (content_json && cJSON_IsString(content_json))
        note_content = str_dup(cJSON_GetStringValue(content_json));

    cJSON_Delete(args);

    if (content_json && cJSON_IsString(content_json) && !note_content)
    {
        free(action);
        free(name);
        return tool_result_error("oom", "execution_error");
    }

    if (strcmp(action, "read") == 0)
    {
        free(action);
        char *fpath = NULL;
        if (asprintf(&fpath, "%s/%s.md", ndir, name) < 0)
        {
            free(name); free(note_content);
            return tool_result_error("oom", "execution_error");
        }

        int fd = open(fpath, O_RDONLY | O_NOFOLLOW);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        {
            if (fd >= 0) close(fd);
            free(fpath); free(name); free(note_content);
            return tool_result_error("note not found", "file_not_found");
        }

        FILE *f = fdopen(fd, "rb");
        if (!f)
        {
            close(fd);
            free(fpath); free(name); free(note_content);
            return tool_result_error("cannot read note", "execution_error");
        }

        if (st.st_size > (long)ctx->safety->max_file_size)
        {
            fclose(f); free(fpath); free(name); free(note_content);
            return tool_result_error("note too large", "policy_denied");
        }

        char *content = malloc((size_t)st.st_size + 1);
        if (!content)
        {
            fclose(f); free(fpath); free(name); free(note_content);
            return tool_result_error("oom", "execution_error");
        }

        size_t read = fread(content, 1, (size_t)st.st_size, f);
        fclose(f);
        content[read] = '\0';
        free(fpath);
        free(name); free(note_content);

        ToolResult *tr = tool_result_create(content);
        free(content);
        return tr;
    }
    else if (strcmp(action, "write") == 0)
    {
        free(action);
        if (!note_content)
        {
            free(name);
            return tool_result_error("missing 'content' for write action", "validation_error");
        }
        if (strlen(note_content) > ctx->safety->max_file_size)
        {
            free(name); free(note_content);
            return tool_result_error("note too large", "policy_denied");
        }

        char *fpath = NULL;
        if (asprintf(&fpath, "%s/%s.md", ndir, name) < 0)
        {
            free(name); free(note_content);
            return tool_result_error("oom", "execution_error");
        }

        int fd = open(fpath, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
        if (!f)
        {
            if (fd >= 0) close(fd);
            free(fpath); free(name); free(note_content);
            return tool_result_error("cannot write note", "execution_error");
        }

        size_t content_len = strlen(note_content);
        int write_failed = fwrite(note_content, 1, content_len, f) != content_len;
        int close_failed = fclose(f) != 0;
        if (write_failed || close_failed)
        {
            free(fpath); free(name); free(note_content);
            return tool_result_error("cannot write note", "execution_error");
        }
        free(fpath);
        free(name); free(note_content);

        return tool_result_create("Note written successfully.");
    }
    else if (strcmp(action, "delete") == 0)
    {
        free(action);
        char *fpath = NULL;
        if (asprintf(&fpath, "%s/%s.md", ndir, name) < 0)
        {
            free(name); free(note_content);
            return tool_result_error("oom", "execution_error");
        }

        int rc = unlink(fpath);
        free(fpath);
        free(name); free(note_content);

        if (rc != 0) return tool_result_error("note not found or cannot delete", "file_not_found");
        return tool_result_create("Note deleted.");
    }

    free(action); free(name); free(note_content);
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

/**
 * tool_notes_create - construct the notes tool
 * @safety: borrowed SafetyConfig consulted on every execution; not owned
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
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
    if (!t->name || !t->description || !t->parameters_schema)
    {
        free(t->name);
        free(t->description);
        free(t->parameters_schema);
        free(ctx);
        free(t);
        return NULL;
    }
    t->execute = notes_execute;
    t->destroy = notes_destroy;
    t->ctx = ctx;
    return t;
}
