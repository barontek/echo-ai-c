/*
 * write_file.c - file writing tool: writes model-provided content to a
 * file, creating missing parent directories (bounded by the workspace
 * root) and snapshotting the pre-write state through the change tracker
 * so edits can be reverted. Depends on: tool.h, safety, change_tracker,
 * string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../change_tracker/change_tracker.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

#ifdef WRITE_FILE_TEST
/* Test-only mkdir seam: lets tests simulate a mkdir failure (e.g. a
 * read-only parent) deterministically. Only the test target defines
 * WRITE_FILE_TEST. */
static int wf_test_mkdir_fail = 0;
void write_file_test_set_mkdir_fail(int fail)
{
    wf_test_mkdir_fail = fail;
}
static int wf_test_mkdir(const char *path, mode_t mode)
{
    if (wf_test_mkdir_fail) return -1;
    return mkdir(path, mode);
}
#define mkdir wf_test_mkdir
#endif

/*
 * write_file_mkdir_p - create a directory path component by component
 * @dir: NUL-terminated directory path, mutated in place (temporarily
 *   split at '/'), restored before return
 *
 * Walks @dir from its first component, calling mkdir(2) on each prefix;
 * an existing directory (EEXIST) is fine, any other failure aborts.
 * Only the given path is created — never anything above it.
 *
 * Return: 0 on success, -1 on failure (a component is not a directory,
 * or mkdir failed for a reason other than EEXIST). The caller's buffer
 * is always restored to its original content.
 */
static int write_file_mkdir_p(char *dir)
{
    char *p = dir;
    if (dir[0] == '/') p = dir + 1;
    for (;;)
    {
        char *sep = strchr(p, '/');
        if (sep) *sep = '\0';
        int rc = mkdir(dir, 0755);
        if (rc != 0 && errno != EEXIST)
        {
            if (sep) *sep = '/';
            return -1;
        }
        if (!sep) break;
        *sep = '/';
        p = sep + 1;
    }
    return 0;
}

/*
 * write_file_ensure_parents - create missing parent directories for a
 * workspace-relative write target
 * @cfg: safety config consulted for the workspace root (restricted mode)
 * @path: write target, relative or absolute; parents may not exist yet
 *
 * Restricted mode: the candidate path is walked from the realpath'd
 * workspace root, creating each missing component with 0755 and
 * realpath-verifying every prefix stays inside the workspace — a
 * symlink planted mid-walk cannot escape the root. Nothing above the
 * workspace root is ever created. Unrestricted mode needs no walk:
 * resolution never fails there, so this is a no-op.
 *
 * Return: 0 on success (or nothing to do), -1 when the target is
 * outside the workspace, contains "..", or any mkdir/realpath step
 * fails. Caller then re-runs safety_resolve_path_alloc() to obtain the
 * canonical final path.
 */
static int write_file_ensure_parents(const SafetyConfig *cfg, const char *path)
{
    if (!cfg || !cfg->workspace || !path) return -1;
    if (cfg->mode == SAFETY_MODE_UNRESTRICTED) return 0;
    if (strstr(path, "..") != NULL) return -1;

    char root[PATH_MAX];
    if (!realpath(cfg->workspace, root)) return -1;

    char candidate[PATH_MAX];
    int written = path[0] == '/'
                      ? snprintf(candidate, sizeof(candidate), "%s", path)
                      : snprintf(candidate, sizeof(candidate), "%s/%s", root, path);
    if (written < 0 || (size_t)written >= sizeof(candidate)) return -1;

    /* The target directory is the candidate minus its basename. */
    char *slash = strrchr(candidate, '/');
    if (!slash || slash[1] == '\0') return -1;
    *slash = '\0';

    /* Refuse targets not rooted inside the workspace. */
    size_t root_len = strlen(root);
    if (strncmp(candidate, root, root_len) != 0 ||
        (candidate[root_len] != '\0' && candidate[root_len] != '/'))
        return -1;

    char prefix[PATH_MAX];
    strlcpy(prefix, root, sizeof(prefix));
    size_t plen = root_len;

    char *rest = candidate + root_len;
    if (*rest == '/') rest++;
    while (*rest)
    {
        char *sep = strchr(rest, '/');
        size_t seglen = sep ? (size_t)(sep - rest) : strlen(rest);
        if (plen + 1 + seglen >= sizeof(prefix)) return -1;
        prefix[plen] = '/';
        memcpy(prefix + plen + 1, rest, seglen);
        prefix[plen + 1 + seglen] = '\0';
        plen += 1 + seglen;

        char check[PATH_MAX];
        if (realpath(prefix, check))
        {
            if (!safety_path_is_within_workspace(cfg, check)) return -1;
        }
        else if (errno == ENOENT)
        {
            if (mkdir(prefix, 0755) != 0 && errno != EEXIST) return -1;
            /* Re-resolve after creation: a symlink appearing between
             * the check and the mkdir must not bypass the root. */
            if (!realpath(prefix, check) ||
                !safety_path_is_within_workspace(cfg, check))
                return -1;
        }
        else
        {
            return -1;
        }

        if (!sep) break;
        rest = sep + 1;
    }
    return 0;
}

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
        /* T2: the parent may not exist yet — create missing components
         * inside the workspace, then resolve again. A policy rejection
         * falls out here too (the walk refuses anything above the
         * workspace root). */
        if (write_file_ensure_parents(ctx->safety, path) == 0)
            resolved = safety_resolve_path_alloc(ctx->safety, path);
        if (!resolved)
        {
            free(path);
            free(content);
            return tool_result_error("path resolution failed",
                                     "execution_error");
        }
    }
    else if (ctx->safety->mode == SAFETY_MODE_UNRESTRICTED)
    {
        /* Unrestricted mode: pi parity — create the parent directories
         * even when resolution succeeds (there is no workspace to pin
         * creation to). */
        char *slash = strrchr(resolved, '/');
        if (slash && slash[1] != '\0')
        {
            *slash = '\0';
            int rc = write_file_mkdir_p(resolved);
            *slash = '/';
            if (rc != 0)
            {
                free(resolved);
                free(path);
                free(content);
                return tool_result_error("cannot create parent directory",
                                         "permission_denied");
            }
        }
    }

    if (ctx->ct && ct_snapshot(ctx->ct, resolved) != 0)
    {
        /* C11: an un-snapshotted edit is permanently un-undoable; the
         * write itself is still correct, but the user loses the undo
         * path. Log so the failure is not invisible. */
        log_error("write_file: ct_snapshot failed (edit will not be "
                  "undoable)", "path", path, NULL);
    }

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
    if (!ctx) {
        free(t);
        return NULL;
    }
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
