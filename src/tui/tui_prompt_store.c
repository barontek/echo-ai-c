/*
 * tui_prompt_store.c - JSONL-backed prompt history and stash. Every
 * entry is one JSON object per line: {"input","mode","ts"}. Reads skip
 * malformed lines; writes rewrite the whole file so caps are exact.
 * Depends on: tui_prompt_store.h, cJSON, string_utils.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cjson/cJSON.h>

#include "tui_prompt_store.h"
#include "../utils/string_utils.h"

/* ---- file helpers ---- */

static char *read_file_alloc(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf)
    {
        fclose(fp); // NOLINT(cert-err33-c)
        return NULL;
    }
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, fp)) > 0) // NOLINT(clang-analyzer-unix.Stream)
    {
        len += n;
        if (len == cap)
        {
            size_t nc = cap * 2;
            char *nb = realloc(buf, nc);
            if (!nb)
            {
                free(buf);
                fclose(fp); // NOLINT(cert-err33-c)
                return NULL;
            }
            buf = nb;
            cap = nc;
        }
    }
    fclose(fp); // NOLINT(cert-err33-c)
    buf[len] = '\0'; // NOLINT(clang-analyzer-security.ArrayBound)
    return buf;
}

static int write_file(const char *path, const char *data)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    size_t n = strlen(data);
    int rc = fwrite(data, 1, n, fp) == n ? 0 : -1;
    if (fclose(fp) != 0) rc = -1;
    return rc;
}

/* One parsed entry (owned strings). */
typedef struct {
    char *input;
    char *mode;
} Entry;

static void entry_free(Entry *e)
{
    if (!e) return;
    free(e->input);
    free(e->mode);
}

static int entry_parse(const char *line, Entry *out)
{
    cJSON *o = cJSON_Parse(line);
    if (!o)
    {
        *out = (Entry){0};
        return -1;
    }
    const char *input = cJSON_GetStringValue(cJSON_GetObjectItem(o, "input"));
    const char *mode = cJSON_GetStringValue(cJSON_GetObjectItem(o, "mode"));
    out->input = input ? str_dup(input) : NULL;
    out->mode = mode ? str_dup(mode) : NULL;
    cJSON_Delete(o);
    if (!input || !out->input)
    {
        entry_free(out);
        *out = (Entry){0};
        return -1;
    }
    return 0;
}

/* Load every parseable entry from the file; missing file = empty list. */
static int entries_load(const char *path, Entry **out, int *out_count)
{
    *out = NULL;
    *out_count = 0;
    char *data = read_file_alloc(path);
    if (!data) return 0; /* missing or empty file is not an error */

    int cap = 16;
    int count = 0;
    Entry *arr = calloc((size_t)cap, sizeof(Entry));
    if (!arr)
    {
        free(data);
        return -1;
    }
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save))
    {
        if (count == cap)
        {
            cap *= 2;
            Entry *nb = realloc(arr, (size_t)cap * sizeof(Entry));
            if (!nb)
            {
                for (int i = 0; i < count; i++) entry_free(&arr[i]);
                free(arr);
                free(data);
                return -1;
            }
            arr = nb;
        }
        if (entry_parse(line, &arr[count]) == 0)
            count++;
    }
    free(data);
    *out = arr;
    *out_count = count;
    return 0;
}

static char *entry_to_json(const char *input, const char *mode)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "input", input ? input : "");
    if (mode && mode[0])
        cJSON_AddStringToObject(o, "mode", mode);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return json;
}

static int entries_write(const char *path, Entry *arr, int count)
{
    if (count == 0)
        return write_file(path, "");
    size_t total = 1U;
    for (int i = 0; i < count; i++)
        total += strlen(arr[i].input) + (arr[i].mode ? strlen(arr[i].mode) : 0) + 64;
    char *buf = malloc(total);
    if (!buf) return -1;
    size_t pos = 0;
    for (int i = 0; i < count; i++)
    {
        char *json = entry_to_json(arr[i].input, arr[i].mode);
        if (!json)
        {
            free(buf);
            return -1;
        }
        pos += (size_t)snprintf(buf + pos, total - pos, "%s\n", json);
        free(json);
    }
    int rc = write_file(path, buf);
    free(buf);
    return rc;
}

/* ---- public API ---- */

int tui_prompt_store_history_append(const char *path, const char *input,
                                    const char *mode, int max_entries)
{
    if (!path || !input) return -1;
    if (max_entries < 1) max_entries = 1;

    Entry *arr = NULL;
    int count = 0;
    if (entries_load(path, &arr, &count) != 0) return -1;

    /* consecutive-duplicate dedup: ignore an exact repeat of the newest */
    if (count > 0 && strcmp(arr[count - 1].input, input) == 0)
    {
        for (int i = 0; i < count; i++) entry_free(&arr[i]);
        free(arr);
        return 0;
    }

    /* append */
    Entry *nb = realloc(arr, (size_t)(count + 1) * sizeof(Entry));
    if (!nb)
    {
        for (int i = 0; i < count; i++) entry_free(&arr[i]);
        free(arr);
        return -1;
    }
    arr = nb;
    arr[count].input = str_dup(input);
    arr[count].mode = mode && mode[0] ? str_dup(mode) : NULL;
    if (!arr[count].input)
    {
        entry_free(&arr[count]);
        for (int i = 0; i < count; i++) entry_free(&arr[i]);
        free(arr);
        return -1;
    }
    count++;

    /* trim to the newest max_entries */
    int start = count > max_entries ? count - max_entries : 0;
    int rc;
    if (start > 0)
    {
        for (int i = 0; i < start; i++) entry_free(&arr[i]);
        memmove(arr, arr + start, (size_t)(count - start) * sizeof(Entry));
        count -= start;
    }
    rc = entries_write(path, arr, count);
    for (int i = 0; i < count; i++) entry_free(&arr[i]);
    free(arr);
    return rc;
}

int tui_prompt_store_history_load(const char *path, char ***out, int *out_count)
{
    if (!path || !out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;
    Entry *arr = NULL;
    int count = 0;
    if (entries_load(path, &arr, &count) != 0) return -1;
    char **strs = calloc((size_t)(count > 0 ? count : 1), sizeof(char *));
    if (!strs)
    {
        for (int i = 0; i < count; i++) entry_free(&arr[i]);
        free(arr);
        return -1;
    }
    for (int i = 0; i < count; i++)
        strs[i] = arr[i].input ? arr[i].input : str_dup("");
    for (int i = 0; i < count; i++) free(arr[i].mode);
    free(arr);
    *out = strs;
    *out_count = count;
    return 0;
}

int tui_prompt_store_stash_push(const char *path, const char *input,
                                int max_entries)
{
    return tui_prompt_store_history_append(path, input, "stash", max_entries);
}

int tui_prompt_store_stash_pop(const char *path, char **out)
{
    if (!path || !out) return -1;
    *out = NULL;
    Entry *arr = NULL;
    int count = 0;
    if (entries_load(path, &arr, &count) != 0) return -1;
    if (count == 0)
    {
        free(arr);
        return 0; /* empty stash: out stays NULL */
    }
    char *popped = str_dup(arr[count - 1].input);
    if (!popped)
    {
        for (int i = 0; i < count; i++) entry_free(&arr[i]);
        free(arr);
        return -1;
    }
    entry_free(&arr[count - 1]);
    int rc = entries_write(path, arr, count - 1);
    for (int i = 0; i < count - 1; i++) entry_free(&arr[i]);
    free(arr);
    if (rc != 0)
    {
        free(popped);
        return -1;
    }
    *out = popped;
    return 0;
}

int tui_prompt_store_stash_list(const char *path, char ***out, int *out_count)
{
    if (!path || !out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;
    Entry *arr = NULL;
    int count = 0;
    if (entries_load(path, &arr, &count) != 0) return -1;
    char **strs = calloc((size_t)(count > 0 ? count : 1), sizeof(char *));
    if (!strs)
    {
        for (int i = 0; i < count; i++) entry_free(&arr[i]);
        free(arr);
        return -1;
    }
    /* newest first, matching the stash picker's ordering */
    for (int i = 0; i < count; i++)
        strs[i] = arr[count - 1 - i].input ? arr[count - 1 - i].input : str_dup("");
    for (int i = 0; i < count; i++) free(arr[i].mode);
    free(arr);
    *out = strs;
    *out_count = count;
    return 0;
}

int tui_prompt_store_stash_remove(const char *path, int index)
{
    if (!path || index < 0) return -1;
    Entry *arr = NULL;
    int count = 0;
    if (entries_load(path, &arr, &count) != 0) return -1;
    /* stash list is newest-first; the file stores oldest-first, so the
     * remove index maps to the reverse position */
    int file_idx = count - 1 - index;
    if (file_idx < 0 || file_idx >= count)
    {
        for (int i = 0; i < count; i++) entry_free(&arr[i]);
        free(arr);
        return -1;
    }
    entry_free(&arr[file_idx]);
    if (file_idx < count - 1)
        memmove(arr + file_idx, arr + file_idx + 1,
                (size_t)(count - 1 - file_idx) * sizeof(Entry));
    int rc = entries_write(path, arr, count - 1);
    for (int i = 0; i < count - 1; i++) entry_free(&arr[i]);
    free(arr);
    return rc;
}