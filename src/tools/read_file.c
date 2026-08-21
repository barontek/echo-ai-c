/*
 * read_file.c - file reading tool: reads a file's contents for the model,
 * bounded by the safety policy's size and path rules, with optional
 * offset/limit line windows and a continuation hint when truncated.
 * Depends on: tool.h, safety, string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

#ifdef READ_FILE_TEST
/* Test-only fread seam: lets tests simulate a short read (file changed
 * under us) deterministically. Only the test target defines
 * READ_FILE_TEST. */
static int rf_test_fread_fail = 0;
void read_file_test_set_fread_fail(int fail)
{
    rf_test_fread_fail = fail;
}
static size_t rf_test_fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (rf_test_fread_fail)
        return nmemb > 0 ? nmemb - 1 : 0;
    return fread(ptr, size, nmemb, stream);
}
#define fread rf_test_fread
#endif

/* Default maximum lines returned without an explicit limit, mirroring
 * pi's read tool; a continuation hint tells the model the next offset. */
#define READ_MAX_LINES 4000

/*
 * read_line_len - length of one line in a '\n'-delimited line array
 * @lines: array of line-start pointers into one buffer, as built by
 *   read_file_execute; must have at least @count entries
 * @count: total number of lines in @lines
 * @idx: index of the line to measure; must be < @count
 *
 * Lines are not NUL-terminated: a line's length is the gap to the next
 * line start minus the '\n' delimiter; the last line runs to the
 * buffer's NUL terminator. strlen() on a line start would run to the
 * end of the file, so callers must use this instead.
 *
 * Return: byte length of line @idx. Never fails; thread-safe on
 * distinct buffers.
 */
static size_t read_line_len(char **lines, size_t count, size_t idx)
{
    if (idx + 1 < count) return (size_t)(lines[idx + 1] - lines[idx]) - 1;
    return strlen(lines[idx]);
}

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

    /* T1: offset (1-indexed) and limit are validated up front so an
     * invalid window errors before any file I/O happens. */
    long offset = 0;
    cJSON *offset_json = cJSON_GetObjectItem(args, "offset");
    if (offset_json)
    {
        double dv = offset_json->valuedouble;
        if (!cJSON_IsNumber(offset_json) || dv < 0.0 ||
            dv > (double)LONG_MAX || dv != (double)(long)dv)
        {
            cJSON_Delete(args);
            return tool_result_error(
                "offset must be a whole number of lines", "validation_error");
        }
        offset = (long)dv;
    }

    long limit = 0;
    cJSON *limit_json = cJSON_GetObjectItem(args, "limit");
    if (limit_json)
    {
        double dv = limit_json->valuedouble;
        if (!cJSON_IsNumber(limit_json) || dv < 1.0 ||
            dv > (double)LONG_MAX || dv != (double)(long)dv)
        {
            cJSON_Delete(args);
            return tool_result_error(
                "limit must be a whole number of lines", "validation_error");
        }
        limit = (long)dv;
    }

    char *path = str_dup(cJSON_GetStringValue(path_json));
    cJSON_Delete(args);

    if (!safety_check_path(ctx->safety, path))
    {
        free(path);
        return tool_result_error("path rejected by safety policy", "policy_denied");
    }

    char *resolved = safety_resolve_path_alloc(ctx->safety, path);
    free(path);
    if (!resolved) return tool_result_error("path resolution failed", "execution_error");

    FILE *fp = fopen(resolved, "r");
    if (!fp)
    {
        free(resolved);
        return tool_result_error("file not found", "file_not_found");
    }

    fseek(fp, 0, SEEK_END); // NOLINT(cert-err33-c)
    long size = ftell(fp);
    if (size < 0 || size > (long)ctx->safety->max_file_size)
    {
        fclose(fp); // NOLINT(cert-err33-c)
        free(resolved);
        return tool_result_error("file exceeds max size", "policy_denied");
    }
    fseek(fp, 0, SEEK_SET); // NOLINT(cert-err33-c)

    char *content = malloc((size_t)size + 1);
    if (!content) {
        fclose(fp); // NOLINT(cert-err33-c)
        free(resolved);
        return NULL;
    }

    size_t read_size = fread(content, 1, (size_t)size, fp);
    fclose(fp); // NOLINT(cert-err33-c)
    free(resolved);
    if (read_size != (size_t)size)
    {
        /* T1e: a short read means the file changed under us; handing
         * the partial contents to the model would be silently wrong. */
        free(content);
        return tool_result_error("read failed: file incomplete",
                                 "execution_error");
    }
    content[read_size] = '\0'; // NOLINT(clang-analyzer-security.ArrayBound)

    /* Split into lines on '\n' (pi semantics: a trailing newline yields
     * a final empty line). */
    size_t cap = 16;
    size_t line_count = 0;
    char **lines = malloc(cap * sizeof(char *));
    if (!lines)
    {
        free(content);
        return tool_result_error("oom", "execution_error");
    }
    lines[line_count++] = content;
    for (size_t i = 0; i < read_size; i++)
    {
        if (content[i] != '\n') continue;
        if (line_count == cap)
        {
            char **grown = realloc(lines, cap * 2 * sizeof(char *));
            if (!grown)
            {
                free(lines);
                free(content);
                return tool_result_error("oom", "execution_error");
            }
            lines = grown;
            cap *= 2;
        }
        lines[line_count++] = content + i + 1;
    }

    size_t start = (size_t)offset > 0 ? (size_t)offset - 1 : 0;
    if (start >= line_count)
    {
        free(lines);
        free(content);
        char *msg = NULL;
        if (asprintf(&msg, "offset beyond end of file (%zu lines total)",
                     line_count) < 0)
            msg = NULL;
        ToolResult *r = tool_result_error(msg, "validation_error");
        free(msg);
        return r;
    }

    size_t avail = line_count - start;
    size_t show = limit > 0 ? (size_t)limit : READ_MAX_LINES;
    if (show > avail) show = avail;

    /* Join the selected lines with '\n' (a k-line join has k-1
     * separators), so an untruncated read returns the original bytes. */
    size_t out_len = 0;
    for (size_t i = 0; i < show; i++)
        out_len += read_line_len(lines, line_count, start + i);
    out_len += show > 0 ? show - 1 : 0;

    char *out = malloc(out_len + 1);
    if (!out)
    {
        free(lines);
        free(content);
        return tool_result_error("oom", "execution_error");
    }
    size_t pos = 0;
    for (size_t i = 0; i < show; i++)
    {
        size_t len = read_line_len(lines, line_count, start + i);
        memcpy(out + pos, lines[start + i], len);
        pos += len;
        if (i + 1 < show) out[pos++] = '\n';
    }
    out[pos] = '\0';

    if (show < avail)
    {
        /* T1: continuation hint with the exact next offset, pi-style. */
        size_t first = start + 1;
        size_t last = start + show;
        char *hint = NULL;
        if (asprintf(&hint,
                     "\n\n[Showing lines %zu-%zu of %zu. "
                     "Use offset=%zu to continue.]",
                     first, last, line_count, last + 1) >= 0)
        {
            if (str_append(&out, hint) != 0)
            {
                free(out);
                out = NULL;
            }
            free(hint);
        }
    }

    free(lines);
    free(content);

    ToolResult *tr = tool_result_create(out);
    free(out);
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
    if (!ctx) {
        free(t);
        return NULL;
    }
    ctx->safety = safety;

    t->name = str_dup("read_file");
    t->description = str_dup("Read contents of a file; optional 1-indexed offset and line limit");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"file_path\":{\"type\":\"string\",\"description\":\"Path to the file\"},"
        "\"offset\":{\"type\":\"number\",\"description\":\"1-indexed line to start reading from\"},"
        "\"limit\":{\"type\":\"number\",\"description\":\"Maximum number of lines to read\"}"
        "},\"required\":[\"file_path\"]}"
    );
    t->execute = read_file_execute;
    t->destroy = read_file_destroy;
    t->ctx = ctx;
    return t;
}
