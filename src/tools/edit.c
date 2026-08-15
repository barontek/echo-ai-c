/*
 * edit.c - edit tool: applies one or more exact text replacements to a
 * file, requiring each old_string to be unique; writes atomically via a
 * temp file + rename. Depends on: tool.h, safety, string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

#ifdef EDIT_TEST
/* Test-only fwrite seam: lets tests simulate a short write (disk full)
 * deterministically. Defined before the #define so its own body calls the
 * real fwrite. Only the test target defines EDIT_TEST. */
static int rif_test_fwrite_fail = 0;
void edit_test_set_fwrite_fail(int fail)
{
    rif_test_fwrite_fail = fail;
}
static size_t rif_test_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (rif_test_fwrite_fail)
    {
        /* Simulate a short write: report one element fewer than asked */
        return nmemb > 0 ? nmemb - 1 : 0;
    }
    return fwrite(ptr, size, nmemb, stream);
}
#define fwrite rif_test_fwrite

/* Test-only str_dup seam (T3d): lets tests fail the Nth duplication to
 * prove the multi-edit commit path rolls back cleanly on OOM. */
static int rif_test_strdup_fail_at = -1;
static int rif_test_strdup_call = 0;
void edit_test_set_strdup_fail(int call)
{
    rif_test_strdup_fail_at = call;
    rif_test_strdup_call = 0;
}
static char *rif_test_str_dup(const char *s)
{
    rif_test_strdup_call++;
    if (rif_test_strdup_call == rif_test_strdup_fail_at) return NULL;
    return str_dup(s);
}
#define str_dup rif_test_str_dup
#endif

typedef struct {
    char *old_str;
    char *new_str;
} EditItem;

typedef struct {
    size_t index;     /* match position in the normalized content */
    size_t length;    /* matched length in the normalized content */
    size_t new_len;   /* normalized length of the replacement text */
    size_t edit_idx;  /* which edit produced this match */
} EditMatch;

typedef struct {
    SafetyConfig *safety;
} EditCtx;

/*
 * edit_count_occurrences - count non-overlapping occurrences of a needle
 * @haystack: NUL-terminated text to search
 * @needle: NUL-terminated text to find; must be non-empty
 * @count_out: receives the occurrence count
 *
 * Return: 0 on success, -1 when @needle is empty (the caller must
 * reject empty old_string before matching). Thread-safe on distinct
 * buffers.
 */
static int edit_count_occurrences(const char *haystack, const char *needle,
                                  size_t *count_out)
{
    size_t count = 0;
    size_t nlen = strlen(needle);
    if (nlen == 0) return -1;
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL)
    {
        count++;
        p += nlen;
    }
    *count_out = count;
    return 0;
}

/*
 * edit_detect_ending - decide whether a text uses CRLF line endings
 * @text: NUL-terminated text (the file content without BOM)
 *
 * The first newline decides (pi's rule): a "\r\n" before the first
 * "\n" means CRLF, anything else means LF.
 *
 * Return: 1 for CRLF, 0 for LF. Never fails.
 */
static int edit_detect_ending(const char *text)
{
    const char *lf = strchr(text, '\n');
    if (!lf) return 0;
    const char *crlf = strstr(text, "\r\n");
    return crlf && crlf < lf;
}

/*
 * edit_normalize_lf_dup - collapse \r\n and lone \r into \n
 * @text: NUL-terminated text to normalize
 *
 * Matching happens in one canonical line-ending space so a model's
 * old_string never fails purely because the file uses CRLF.
 *
 * Return: freshly malloc'd normalized copy owned by the caller (free
 * with free()), or NULL on OOM.
 */
static char *edit_normalize_lf_dup(const char *text)
{
    size_t len = strlen(text);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (text[i] == '\r')
        {
            out[o++] = '\n';
            if (i + 1 < len && text[i + 1] == '\n') i++;
        }
        else
        {
            out[o++] = text[i];
        }
    }
    out[o] = '\0';
    return out;
}

/*
 * edit_restore_endings_dup - expand \n to \r\n when the file was CRLF
 * @text: NUL-terminated LF-normalized text
 * @crlf: 1 to expand, 0 to return a plain copy
 *
 * Return: freshly malloc'd copy owned by the caller, or NULL on OOM.
 */
static char *edit_restore_endings_dup(const char *text, int crlf)
{
    if (!crlf) return str_dup(text);
    size_t count = 0;
    for (const char *p = text; *p; p++)
        if (*p == '\n') count++;
    size_t len = strlen(text) + count;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (const char *p = text; *p; p++)
    {
        if (*p == '\n')
        {
            out[o++] = '\r';
            out[o++] = '\n';
        }
        else
        {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
    return out;
}

/*
 * edit_write_atomic - write data via temp file + atomic rename
 * @resolved: canonical destination path
 * @data: bytes to write; @len: byte count
 * @written_out: receives the byte count written on success
 *
 * An interrupted/short write cannot destroy the only copy of the
 * original content (rename is atomic on POSIX); the temp file is
 * unlinked on any failure.
 *
 * Return: 0 on success, -1 on failure (short write or rename), -2 when
 * the temp file could not be created. Never leaves @resolved modified
 * on failure.
 */
static int edit_write_atomic(const char *resolved, const char *data,
                             size_t len, size_t *written_out)
{
    char *tmp_path = NULL;
    if (asprintf(&tmp_path, "%s.tmp.%ld", resolved, (long)getpid()) < 0)
        return -1;

    FILE *f = fopen(tmp_path, "wb");
    if (!f)
    {
        free(tmp_path);
        return -2;
    }

    size_t written = fwrite(data, 1, len, f);
    if (written != len || fclose(f) != 0)
    {
        /* C2: a short write (disk full) silently truncated the file and
         * the tool reported success. Report the failure; the original
         * file is untouched. */
        unlink(tmp_path);
        free(tmp_path);
        return -1;
    }
    if (rename(tmp_path, resolved) != 0)
    {
        log_error("edit: rename failed", "err", strerror(errno), NULL);
        unlink(tmp_path);
        free(tmp_path);
        return -1;
    }
    free(tmp_path);
    *written_out = written;
    return 0;
}

/* --- Result diff (opencode-style) ---
 * The result carries a line diff of the change so the TUI shows the
 * edit the way opencode's diff viewer does. Context lines are prefixed
 * with their line number, changed lines with '-'/'+' and their
 * old/new line numbers, " ..." marks skipped runs. */

#define EDIT_DIFF_CONTEXT 3

typedef struct {
    char **lines; /* line starts; '\n'-delimited, last line NUL-ended */
    size_t *lens; /* line lengths excluding the '\n' */
    size_t count;
} EditLines;

static void edit_lines_free(EditLines *el)
{
    free(el->lines);
    free(el->lens);
    el->lines = NULL;
    el->lens = NULL;
    el->count = 0;
}

/*
 * edit_lines_build - split a NUL-terminated text into '\n'-delimited lines
 * @text: text to split (not owned; the line pointers alias it)
 * @el: receives the line array and per-line lengths
 *
 * A trailing newline yields a final empty line (same convention as the
 * read tool). The line pointers alias @text, which must outlive @el.
 *
 * Return: 0 on success, -1 on OOM (@el left empty).
 */
static int edit_lines_build(const char *text, EditLines *el)
{
    size_t len = strlen(text);
    size_t cap = 16;
    el->count = 0;
    el->lines = malloc(cap * sizeof(char *));
    el->lens = malloc(cap * sizeof(size_t));
    if (!el->lines || !el->lens)
    {
        edit_lines_free(el);
        return -1;
    }
    el->lines[0] = (char *)text;
    el->count = 1;
    for (size_t i = 0; i < len; i++)
    {
        if (text[i] != '\n') continue;
        if (el->count == cap)
        {
            size_t ncap = cap * 2;
            char **nl = realloc(el->lines, ncap * sizeof(char *));
            size_t *nlens = realloc(el->lens, ncap * sizeof(size_t));
            if (!nl || !nlens)
            {
                free(nl);
                free(nlens);
                edit_lines_free(el);
                return -1;
            }
            el->lines = nl;
            el->lens = nlens;
            cap = ncap;
        }
        el->lines[el->count] = (char *)text + i + 1;
        el->count++;
    }
    for (size_t i = 0; i + 1 < el->count; i++)
        el->lens[i] = (size_t)(el->lines[i + 1] - el->lines[i]) - 1;
    el->lens[el->count - 1] = strlen(el->lines[el->count - 1]);
    return 0;
}

/* One maximal run of changed lines: [old_start, old_end) in the old
 * text, [new_start, new_end) in the new text. */
typedef struct {
    size_t old_start;
    size_t old_end;
    size_t new_start;
    size_t new_end;
} EditRun;

/*
 * edit_line_norm_len - normalized length of one line
 * @line/@len: line bytes (may contain \r\n or lone \r)
 *
 * A "\r\n" pair normalizes to a single "\n"; everything else counts
 * as itself. Used to map normalized match offsets back to line
 * numbers in the real (possibly CRLF) text.
 *
 * Return: the line's length after edit_normalize_lf_dup().
 */
static size_t edit_line_norm_len(const char *line, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++)
    {
        n++;
        if (line[i] == '\r' && i + 1 < len && line[i + 1] == '\n')
            i++;
    }
    return n;
}

/* Cumulative normalized line starts: starts[i] is the normalized
 * offset of line i's first byte; starts[count] is the total. */
typedef struct {
    size_t *starts;
    size_t count;
} EditNormMap;

static void edit_norm_map_free(EditNormMap *m)
{
    free(m->starts);
    m->starts = NULL;
    m->count = 0;
}

static int edit_norm_map_build(const EditLines *el, EditNormMap *m)
{
    m->count = el->count;
    m->starts = malloc((el->count + 1) * sizeof(size_t));
    if (!m->starts) return -1;
    size_t acc = 0;
    for (size_t i = 0; i < el->count; i++)
    {
        m->starts[i] = acc;
        size_t nl = edit_line_norm_len(el->lines[i], el->lens[i]);
        if (i + 1 < el->count)
        {
            /* the '\n' delimiter is one normalized byte — unless it
             * pairs with a trailing '\r' that edit_line_norm_len()
             * already counted as the pair's single '\n' */
            if (el->lens[i] == 0 || el->lines[i][el->lens[i] - 1] != '\r')
                nl++;
        }
        acc += nl;
    }
    m->starts[el->count] = acc;
    return 0;
}

/* Index of the line containing normalized offset @p (binary search;
 * @p is always within [0, total) for offsets derived from matches). */
static size_t edit_norm_line_of(const EditNormMap *m, size_t p)
{
    size_t lo = 0;
    size_t hi = m->count;
    while (lo + 1 < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        if (m->starts[mid] <= p) lo = mid;
        else hi = mid;
    }
    return lo;
}

/*
 * edit_matches_to_runs - convert matched edits to exact line runs
 * @matches: sorted matches (by position), each with its normalized
 *   span in the original content
 * @nmatch: match count
 * @old/@neu: line lists of the before/after content
 * @om/@nm: normalized line maps of the before/after content
 * @runs_out/@n_out: caller-owned run array (free with free())
 *
 * Every edit's old_string matched at normalized [P, P+L) in the
 * original; because replacements are applied in reverse order, the
 * replacement occupies [P, P+N) in the final content. Each span maps
 * to exact line ranges in both texts, so a multi-line removal renders
 * exactly the removed lines — no resync heuristics that can swallow
 * the rest of the file when the alignment shifts (the greedy
 * collector's failure mode). Runs whose context windows would touch
 * are merged.
 *
 * Return: 0 on success, -1 on OOM.
 */
static int edit_matches_to_runs(const EditMatch *matches, size_t nmatch,
                                const EditNormMap *om, const EditNormMap *nm,
                                EditRun **runs_out, size_t *n_out)
{
    EditRun *runs = malloc((nmatch ? nmatch : 1) * sizeof(EditRun));
    if (!runs) return -1;
    size_t n = 0;
    /* Edits are applied in reverse position order, so each replacement
     * lands at its original position plus the net size change of every
     * edit positioned before it (those are applied later and shift the
     * content after them). */
    long shift = 0;
    for (size_t i = 0; i < nmatch; i++)
    {
        size_t p = matches[i].index;
        long p_new = (long)matches[i].index + shift;
        size_t pn = p_new >= 0 ? (size_t)p_new : 0; /* defensive */
        size_t o_start = edit_norm_line_of(om, p);
        size_t o_end = edit_norm_line_of(om, p + matches[i].length - 1) + 1;
        size_t n_start = edit_norm_line_of(nm, pn);
        size_t n_end = matches[i].new_len > 0
                           ? edit_norm_line_of(nm, pn + matches[i].new_len - 1) + 1
                           : n_start;
        shift += (long)matches[i].new_len - (long)matches[i].length;
        if (n > 0 &&
            o_start <= runs[n - 1].old_end + EDIT_DIFF_CONTEXT * 2 &&
            n_start <= runs[n - 1].new_end + EDIT_DIFF_CONTEXT * 2)
        {
            runs[n - 1].old_end = o_end;
            runs[n - 1].new_end = n_end;
        }
        else
        {
            runs[n].old_start = o_start;
            runs[n].old_end = o_end;
            runs[n].new_start = n_start;
            runs[n].new_end = n_end;
            n++;
        }
    }
    *runs_out = runs;
    *n_out = n;
    return 0;
}

/* Append one diff line: prefix (' ' / '-' / '+'), line number, content.
 * A trailing '\r' (from a CRLF file) is trimmed for display. */
static int edit_append_diff_line(char **out, char prefix, size_t num,
                                 const char *line, size_t len)
{
    if (len > 0 && line[len - 1] == '\r') len--;
    char *tmp = NULL;
    if (asprintf(&tmp, "%c%zu %.*s\n", prefix, num, (int)len, line) < 0)
        return -1;
    int rc = str_append(out, tmp);
    free(tmp);
    return rc;
}

/*
 * edit_build_diff - render the changed-line diff of an edit
 * @old_text/@new_text: before/after contents (without BOM)
 * @matches: sorted matches with their normalized spans (see
 *   edit_matches_to_runs); must be sorted by position
 * @nmatch: match count
 * @out: receives the malloc'd diff text, or NULL on OOM
 *
 * Format (pi-style, terminal-friendly): context lines are " N text",
 * removed lines "-N text", added lines "+N text", with at most
 * EDIT_DIFF_CONTEXT lines of context directly above and below each
 * change; skipped regions are visible from the line numbers alone.
 *
 * Return: 0 on success, -1 on OOM (the caller may then omit the diff
 * from the result — it is a display nicety, not a correctness
 * requirement).
 */
static int edit_build_diff(const char *old_text, const char *new_text,
                           const EditMatch *matches, size_t nmatch,
                           char **out)
{
    EditLines old;
    EditLines neu;
    if (edit_lines_build(old_text, &old) != 0) return -1;
    if (edit_lines_build(new_text, &neu) != 0)
    {
        edit_lines_free(&old);
        return -1;
    }

    EditNormMap om;
    EditNormMap nm;
    if (edit_norm_map_build(&old, &om) != 0 ||
        edit_norm_map_build(&neu, &nm) != 0)
    {
        edit_norm_map_free(&om);
        edit_norm_map_free(&nm);
        edit_lines_free(&old);
        edit_lines_free(&neu);
        return -1;
    }

    EditRun *runs = NULL;
    size_t n = 0;
    if (edit_matches_to_runs(matches, nmatch, &om, &nm, &runs, &n) != 0)
    {
        edit_norm_map_free(&om);
        edit_norm_map_free(&nm);
        edit_lines_free(&old);
        edit_lines_free(&neu);
        return -1;
    }
    edit_norm_map_free(&om);
    edit_norm_map_free(&nm);

    char *diff = NULL;
    int rc = 0;
    size_t old_i = 0; /* next old index to emit (context walk) */
    for (size_t r = 0; r < n && rc == 0; r++)
    {
        const EditRun *run = &runs[r];

        /* unchanged gap before this run. Every change gets its own 3
         * context lines above and below: a gap before the first change
         * shows the 3 lines right above it; a gap between two changes
         * shows the 3 lines below the previous change plus the 3 lines
         * above this one. The line numbers in the context make skipped
         * regions obvious, so no " ..." markers are emitted. Gaps small
         * enough to be contiguous context are shown whole. */
        if (run->old_start > old_i)
        {
            size_t gap = run->old_start - old_i;
            if (gap <= EDIT_DIFF_CONTEXT * 2)
            {
                for (size_t k = 0; k < gap; k++)
                {
                    rc = edit_append_diff_line(&diff, ' ', old_i + k + 1,
                                               old.lines[old_i + k],
                                               old.lens[old_i + k]);
                    if (rc != 0) break;
                }
            }
            else
            {
                if (r > 0)
                {
                    for (size_t k = 0; k < EDIT_DIFF_CONTEXT; k++)
                    {
                        rc = edit_append_diff_line(&diff, ' ', old_i + k + 1,
                                                   old.lines[old_i + k],
                                                   old.lens[old_i + k]);
                        if (rc != 0) break;
                    }
                }
                for (size_t k = 0; k < EDIT_DIFF_CONTEXT; k++)
                {
                    size_t idx = run->old_start - EDIT_DIFF_CONTEXT + k;
                    rc = edit_append_diff_line(&diff, ' ', idx + 1,
                                               old.lines[idx], old.lens[idx]);
                    if (rc != 0) break;
                }
            }
            old_i += gap;
            if (rc != 0) break;
        }

        /* the change itself */
        for (size_t k = run->old_start; k < run->old_end; k++)
        {
            rc = edit_append_diff_line(&diff, '-', k + 1,
                                       old.lines[k], old.lens[k]);
            if (rc != 0) break;
        }
        for (size_t k = run->new_start; k < run->new_end; k++)
        {
            rc = edit_append_diff_line(&diff, '+', k + 1,
                                       neu.lines[k], neu.lens[k]);
            if (rc != 0) break;
        }
        old_i = run->old_end;
    }

    /* unchanged tail after the last run: at most EDIT_DIFF_CONTEXT
     * lines, no skip marker */
    if (rc == 0 && old_i < old.count)
    {
        size_t rest = old.count - old_i;
        size_t show = rest > EDIT_DIFF_CONTEXT ? EDIT_DIFF_CONTEXT : rest;
        for (size_t k = 0; k < show; k++)
        {
            rc = edit_append_diff_line(&diff, ' ', old_i + k + 1,
                                       old.lines[old_i + k],
                                       old.lens[old_i + k]);
            if (rc != 0) break;
        }
    }

    free(runs);
    edit_lines_free(&old);
    edit_lines_free(&neu);
    if (rc != 0)
    {
        free(diff);
        return -1;
    }
    if (diff && diff[0] != '\0')
    {
        /* pi's diff string joins lines with '\n' — no trailing one */
        size_t dlen = strlen(diff);
        if (diff[dlen - 1] == '\n') diff[dlen - 1] = '\0';
    }
    *out = diff ? diff : str_dup("");
    if (!*out) return -1;
    return 0;
}

static ToolResult *edit_execute(Tool *self, const char *args_json)
{
    EditCtx *ctx = self->ctx;
    ToolResult *r = NULL;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *path_json = cJSON_GetObjectItem(args, "path");
    if (!path_json || !cJSON_IsString(path_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing required argument: path",
                                 "validation_error");
    }

    /* T3: the contract is an edits[] array; the flat old_string/
     * new_string form is accepted for backward compatibility and folded
     * into a single edit (pi's prepareEditArguments does the same). */
    cJSON *edits_json = cJSON_GetObjectItem(args, "edits");
    cJSON *old_json = cJSON_GetObjectItem(args, "old_string");
    cJSON *new_json = cJSON_GetObjectItem(args, "new_string");

    size_t n = 0;
    int legacy = 0;
    size_t i;
    if (cJSON_IsArray(edits_json))
    {
        n = (size_t)cJSON_GetArraySize(edits_json);
    }
    else if (cJSON_IsString(old_json) && cJSON_IsString(new_json))
    {
        n = 1;
        legacy = 1;
    }

    if (n == 0)
    {
        cJSON_Delete(args);
        return tool_result_error(
            "missing required arguments: a non-empty edits array, or "
            "old_string and new_string",
            "validation_error");
    }

    char *path = str_dup(cJSON_GetStringValue(path_json));
    if (!path)
    {
        cJSON_Delete(args);
        return tool_result_error("oom", "execution_error");
    }

    EditItem *items = calloc(n, sizeof(EditItem));
    if (!items)
    {
        free(path);
        cJSON_Delete(args);
        return tool_result_error("oom", "execution_error");
    }

    /* Pass 1: validate the shape of every edit while the JSON is alive. */
    for (i = 0; i < n; i++)
    {
        if (legacy) continue;
        cJSON *item = cJSON_GetArrayItem(edits_json, (int)i);
        cJSON *o = item ? cJSON_GetObjectItem(item, "old_string") : NULL;
        cJSON *w = item ? cJSON_GetObjectItem(item, "new_string") : NULL;
        if (!item || !cJSON_IsObject(item) || !o || !cJSON_IsString(o) ||
            !w || !cJSON_IsString(w))
            break;
    }
    if (i < n)
    {
        char *msg = NULL;
        if (asprintf(&msg,
                     "edits[%zu] must be an object with string old_string "
                     "and new_string",
                     i) < 0)
            msg = NULL;
        r = tool_result_error(msg, "validation_error");
        free(msg);
        goto cleanup_items_path;
    }

    /* Pass 2: duplicate the strings (allocation-failure safe). */
    for (i = 0; i < n; i++)
    {
        const char *o;
        const char *w;
        if (legacy)
        {
            o = cJSON_GetStringValue(old_json);
            w = cJSON_GetStringValue(new_json);
        }
        else
        {
            cJSON *item = cJSON_GetArrayItem(edits_json, (int)i);
            o = cJSON_GetStringValue(cJSON_GetObjectItem(item, "old_string"));
            w = cJSON_GetStringValue(cJSON_GetObjectItem(item, "new_string"));
        }
        items[i].old_str = str_dup(o);
        items[i].new_str = str_dup(w);
        if (!items[i].old_str || !items[i].new_str) break;
    }
    cJSON_Delete(args);

    if (i < n)
    {
        r = tool_result_error("oom", "execution_error");
        goto cleanup_items_path;
    }

    for (i = 0; i < n; i++)
    {
        if (items[i].old_str[0] != '\0') continue;
        char *msg = NULL;
        if (asprintf(&msg, "edits[%zu].old_string must not be empty", i) < 0)
            msg = NULL;
        r = tool_result_error(msg, "validation_error");
        free(msg);
        goto cleanup_items_path;
    }

    if (!safety_check_path(ctx->safety, path))
    {
        r = tool_result_error("path rejected by safety check",
                              "policy_denied");
        goto cleanup_items_path;
    }

    char *resolved = safety_resolve_path_alloc(ctx->safety, path);
    if (!resolved)
    {
        r = tool_result_error("path resolution failed", "policy_denied");
        goto cleanup_items_path;
    }

    FILE *f = fopen(resolved, "rb");
    if (!f)
    {
        r = tool_result_error("file not found", "file_not_found");
        free(resolved);
        goto cleanup_items_path;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    if (fsize <= 0 || (size_t)fsize > ctx->safety->max_file_size)
    {
        fclose(f);
        r = tool_result_error("file empty or too large", "policy_denied");
        free(resolved);
        goto cleanup_items_path;
    }

    char *content = malloc((size_t)fsize + 1);
    if (!content)
    {
        fclose(f);
        r = tool_result_error("oom", "execution_error");
        free(resolved);
        goto cleanup_items_path;
    }

    size_t read = fread(content, 1, (size_t)fsize, f);
    fclose(f);
    if (read != (size_t)fsize)
    {
        /* C5: a short read means the file changed under us; rewriting it
         * would silently truncate. */
        free(content);
        r = tool_result_error("read failed: file incomplete",
                              "execution_error");
        free(resolved);
        goto cleanup_items_path;
    }
    content[read] = '\0';

    /* T3a: strip a UTF-8 BOM before matching (the model will not echo it
     * back) and remember the file's line ending so the rewritten file
     * keeps it. */
    const char *text = content;
    const char *bom = "";
    if (read >= 3 && (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB &&
        (unsigned char)content[2] == 0xBF)
    {
        bom = content;
        text = content + 3;
    }
    int crlf = edit_detect_ending(text);
    char *norm = edit_normalize_lf_dup(text);
    if (!norm)
    {
        free(content);
        r = tool_result_error("oom", "execution_error");
        free(resolved);
        goto cleanup_items_path;
    }

    /* T3: match every old_string against the ORIGINAL content (all
     * edits are matched before any is applied), require uniqueness,
     * reject overlaps, then apply in reverse position order. */
    EditMatch *matches = calloc(n, sizeof(EditMatch));
    if (!matches)
    {
        free(norm);
        free(content);
        r = tool_result_error("oom", "execution_error");
        free(resolved);
        goto cleanup_items_path;
    }

    size_t matched = 0;
    for (i = 0; i < n; i++)
    {
        char *norm_old = edit_normalize_lf_dup(items[i].old_str);
        if (!norm_old)
        {
            free(matches);
            free(norm);
            free(content);
            r = tool_result_error("oom", "execution_error");
            free(resolved);
            goto cleanup_items_path;
        }

        size_t count = 0;
        if (edit_count_occurrences(norm, norm_old, &count) != 0)
        {
            /* Unreachable: empty old_string was rejected above. */
            free(norm_old);
            free(matches);
            free(norm);
            free(content);
            r = tool_result_error("oom", "execution_error");
            free(resolved);
            goto cleanup_items_path;
        }

        if (count == 0)
        {
            char *msg = NULL;
            if (n == 1)
            {
                if (asprintf(&msg,
                             "Could not find the exact text in %s. The "
                             "old_string must match exactly including "
                             "all whitespace and newlines.",
                             path) < 0)
                    msg = NULL;
            }
            else
            {
                if (asprintf(&msg,
                             "Could not find edits[%zu] in %s. The "
                             "old_string must match exactly including "
                             "all whitespace and newlines.",
                             i, path) < 0)
                    msg = NULL;
            }
            free(norm_old);
            free(matches);
            free(norm);
            free(content);
            r = tool_result_error(msg, "validation_error");
            free(msg);
            free(resolved);
            goto cleanup_items_path;
        }

        if (count > 1)
        {
            /* T3: a duplicate old_string is refused with the occurrence
             * count so the model can add context; the old behavior
             * silently replaced the first occurrence. */
            char *msg = NULL;
            if (n == 1)
            {
                if (asprintf(&msg,
                             "Found %zu occurrences of the text in %s. "
                             "The old_string must be unique; provide more "
                             "context to make it unique.",
                             count, path) < 0)
                    msg = NULL;
            }
            else
            {
                if (asprintf(&msg,
                             "Found %zu occurrences of edits[%zu] in %s. "
                             "Each old_string must be unique; provide more "
                             "context to make it unique.",
                             count, i, path) < 0)
                    msg = NULL;
            }
            free(norm_old);
            free(matches);
            free(norm);
            free(content);
            r = tool_result_error(msg, "validation_error");
            free(msg);
            free(resolved);
            goto cleanup_items_path;
        }

        matches[matched].index = (size_t)(strstr(norm, norm_old) - norm);
        matches[matched].length = strlen(norm_old);
        matches[matched].new_len = edit_line_norm_len(
            items[i].new_str, strlen(items[i].new_str));
        matches[matched].edit_idx = i;
        matched++;
        free(norm_old);
    }

    /* Sort by position so the overlap check and the reverse-order
     * application see content order, not call order. */
    for (i = 1; i < matched; i++)
    {
        EditMatch m = matches[i];
        size_t j = i;
        while (j > 0 && matches[j - 1].index > m.index)
        {
            matches[j] = matches[j - 1];
            j--;
        }
        matches[j] = m;
    }

    for (i = 1; i < matched; i++)
    {
        if (matches[i - 1].index + matches[i - 1].length >
            matches[i].index)
        {
            char *msg = NULL;
            if (asprintf(&msg,
                         "edits[%zu] and edits[%zu] overlap in %s. Merge "
                         "them into one edit or target disjoint regions.",
                         matches[i - 1].edit_idx, matches[i].edit_idx,
                         path) < 0)
                msg = NULL;
            free(matches);
            free(norm);
            free(content);
            r = tool_result_error(msg, "validation_error");
            free(msg);
            free(resolved);
            goto cleanup_items_path;
        }
    }

    /* Apply in reverse position order so earlier offsets stay valid. */
    char *new_norm = str_dup(norm);
    if (!new_norm)
    {
        free(matches);
        free(norm);
        free(content);
        r = tool_result_error("oom", "execution_error");
        free(resolved);
        goto cleanup_items_path;
    }

    for (i = matched; i > 0; i--)
    {
        EditMatch *m = &matches[i - 1];
        char *nnew = edit_normalize_lf_dup(items[m->edit_idx].new_str);
        if (!nnew)
        {
            free(new_norm);
            free(matches);
            free(norm);
            free(content);
            r = tool_result_error("oom", "execution_error");
            free(resolved);
            goto cleanup_items_path;
        }
        size_t nlen = strlen(nnew);
        size_t tail = strlen(new_norm) - m->index - m->length;
        size_t nsize = m->index + nlen + tail + 1;
        char *repl = malloc(nsize);
        if (!repl)
        {
            free(nnew);
            free(new_norm);
            free(matches);
            free(norm);
            free(content);
            r = tool_result_error("oom", "execution_error");
            free(resolved);
            goto cleanup_items_path;
        }
        memcpy(repl, new_norm, m->index);
        memcpy(repl + m->index, nnew, nlen);
        memcpy(repl + m->index + nlen, new_norm + m->index + m->length,
               tail + 1);
        free(new_norm);
        new_norm = repl;
        free(nnew);
    }
    /* matches stay alive until the diff is built: the diff maps each
     * match's normalized span onto exact line ranges */

    /* Restore the original line endings and BOM, then refuse a no-op
     * write (the replacement produced identical content). */
    char *restored = edit_restore_endings_dup(new_norm, crlf);
    free(new_norm);
    if (!restored)
    {
        free(norm);
        free(content);
        r = tool_result_error("oom", "execution_error");
        free(resolved);
        goto cleanup_items_path;
    }

    size_t bom_len = bom[0] != '\0' ? 3 : 0;
    size_t final_len = bom_len + strlen(restored);
    char *final = malloc(final_len + 1);
    if (!final)
    {
        free(restored);
        free(norm);
        free(content);
        r = tool_result_error("oom", "execution_error");
        free(resolved);
        goto cleanup_items_path;
    }
    memcpy(final, bom, bom_len);
    memcpy(final + bom_len, restored, strlen(restored) + 1);

    if (final_len == (size_t)fsize && memcmp(final, content, final_len) == 0)
    {
        free(final);
        free(restored);
        free(norm);
        free(content);
        free(matches);
        r = tool_result_error(
            "No changes made: the replacement produced identical content.",
            "validation_error");
        free(resolved);
        goto cleanup_items_path;
    }

    /* opencode-style result: carry the line diff of the change so the
     * TUI shows the edit like a diff viewer. Non-fatal on OOM — the
     * result then simply omits it. */
    char *diff_text = NULL;
    if (edit_build_diff(text, restored, matches, matched, &diff_text) != 0)
        diff_text = NULL;
    free(matches);
    free(restored);
    free(norm);
    free(content);

    size_t written = 0;
    int wr = edit_write_atomic(resolved, final, final_len, &written);
    free(final);
    if (wr != 0)
    {
        r = tool_result_error(
            wr == -2 ? "cannot write file"
                     : "write failed: file may be incomplete",
            "execution_error");
        free(diff_text);
        free(resolved);
        goto cleanup_items_path;
    }

    char *result = NULL;
    if (n == 1)
    {
        if (asprintf(&result, "Replaced 1 occurrence (%zu bytes written).",
                     written) < 0)
            result = str_dup("Replaced 1 occurrence.");
    }
    else
    {
        if (asprintf(&result, "Replaced %zu occurrences (%zu bytes written).",
                     n, written) < 0)
            result = str_dup("Replaced multiple occurrences.");
    }
    if (result && diff_text)
    {
        char *with_diff = NULL;
        if (asprintf(&with_diff, "%s\n\n%s", result, diff_text) >= 0)
        {
            free(result);
            result = with_diff;
        }
    }
    free(diff_text);

    free(resolved);
    free(path);
    for (i = 0; i < n; i++)
    {
        free(items[i].old_str);
        free(items[i].new_str);
    }
    free(items);

    ToolResult *tr = tool_result_create(result);
    free(result);
    return tr;

cleanup_items_path:
    free(path);
    for (i = 0; i < n; i++)
    {
        free(items[i].old_str);
        free(items[i].new_str);
    }
    free(items);
    return r;
}

static void edit_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_edit_create - construct the edit tool
 * @safety: borrowed SafetyConfig consulted on every execution; not owned
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
Tool *tool_edit_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    EditCtx *ctx = calloc(1, sizeof(EditCtx));
    if (!ctx) {
        free(t);
        return NULL;
    }
    ctx->safety = safety;

    t->name = str_dup("edit");
    t->description = str_dup("Edit a file using exact text replacement; one or more edits, each old_string must be unique in the file");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\",\"description\":\"File path\"},"
        "\"edits\":{\"type\":\"array\",\"description\":\"One or more targeted replacements; each old_string must be unique in the file and edits must not overlap\",\"items\":{\"type\":\"object\",\"properties\":{\"old_string\":{\"type\":\"string\",\"description\":\"Exact text to find\"},\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},\"required\":[\"old_string\",\"new_string\"]}},"
        "\"old_string\":{\"type\":\"string\",\"description\":\"Legacy single-edit form\"},"
        "\"new_string\":{\"type\":\"string\",\"description\":\"Legacy single-edit form\"}"
        "},\"required\":[\"path\"]}"
    );
    t->execute = edit_execute;
    t->destroy = edit_destroy;
    t->ctx = ctx;
    return t;
}
