/*
 * tui_model_store.c - JSON-backed recent/favorite model lists. Both lists
 * are loaded together and rewritten together so one never clobbers the
 * other.
 * Depends on: tui_model_store.h, cJSON, string_utils.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "tui_model_store.h"
#include "../utils/string_utils.h"

#ifdef TUI_MODEL_STORE_TEST
/* Test-only fault-injection seam (AGENTS.md "Fault-injection testing"):
 * force a calloc failure at a chosen call so the load/record/toggle
 * commit paths can be proven clean under OOM. Per-target definition only.
 */
static int test_alloc_fail_at = -1;
static int test_alloc_call_count = 0;
static void *test_calloc(size_t nmemb, size_t size)
{
    test_alloc_call_count++;
    if (test_alloc_call_count == test_alloc_fail_at) return NULL;
    return calloc(nmemb, size);
}
#define calloc test_calloc
void tui_model_store_test_set_alloc_fail(int nth_allocation)
{
    test_alloc_fail_at = nth_allocation;
    test_alloc_call_count = 0;
}
#endif

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

/* Parse a string array from a JSON object key. */
static int parse_list(cJSON *root, const char *key, char ***out, int *out_count)
{
    *out = NULL;
    *out_count = 0;
    cJSON *arr = cJSON_GetObjectItem(root, key);
    int count = arr && cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    char **list = calloc((size_t)(count > 0 ? count : 1), sizeof(char *));
    if (!list) return -1;
    int n = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, arr)
    {
        const char *s = cJSON_GetStringValue(item);
        if (!s) continue;
        list[n] = str_dup(s); // NOLINT(clang-analyzer-security.ArrayBound)
        if (!list[n])
        {
            for (int i = 0; i < n; i++) free(list[i]);
            free(list);
            return -1;
        }
        n++;
    }
    *out = list;
    *out_count = n;
    return 0;
}

static void free_list(char **list, int count)
{
    if (!list) return;
    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
}

int tui_model_store_load(const char *path, char ***recent, int *recent_count,
                         char ***favorites, int *favorite_count)
{
    if (!path || !recent || !recent_count || !favorites || !favorite_count)
        return -1;
    /* caller frees the empty arrays even when nothing is stored */
    *recent = calloc(1, sizeof(char *));
    *favorites = calloc(1, sizeof(char *));
    if (!*recent || !*favorites)
    {
        free(*recent);
        free(*favorites);
        *recent = NULL;
        *favorites = NULL;
        return -1;
    }
    *recent_count = 0;
    *favorite_count = 0;
    char *data = read_file_alloc(path);
    if (!data) return 0;

    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root) return 0;

    int rc = 0;
    char **r = NULL, **f = NULL;
    int rn = 0, fn = 0;
    if (parse_list(root, "recent", &r, &rn) != 0) rc = -1;
    if (parse_list(root, "favorites", &f, &fn) != 0) rc = -1;
    cJSON_Delete(root);
    if (rc == 0)
    {
        free(*recent);
        free(*favorites);
        *recent = r;
        *recent_count = rn;
        *favorites = f;
        *favorite_count = fn;
    }
    else
    {
        free_list(r, rn);
        free_list(f, fn);
        free(*recent);
        free(*favorites);
        *recent = NULL;
        *favorites = NULL;
    }
    return rc;
}

static int store_write(const char *path, const char *const *recent,
                       int recent_count, const char *const *favorites,
                       int favorite_count)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON *r = cJSON_CreateArray();
    cJSON *f = cJSON_CreateArray();
    if (!r || !f)
    {
        if (r) cJSON_Delete(r);
        if (f) cJSON_Delete(f);
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddItemToObject(root, "recent", r);
    cJSON_AddItemToObject(root, "favorites", f);
    for (int i = 0; i < recent_count; i++)
        if (recent && recent[i])
            cJSON_AddItemToArray(r, cJSON_CreateString(recent[i]));
    for (int i = 0; i < favorite_count; i++)
        if (favorites && favorites[i])
            cJSON_AddItemToArray(f, cJSON_CreateString(favorites[i]));
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return -1;
    int rc = write_file(path, json);
    free(json);
    return rc;
}

static int list_contains(const char *const *list, int count, const char *id)
{
    for (int i = 0; i < count; i++)
        if (list[i] && strcmp(list[i], id) == 0)
            return 1;
    return 0;
}

int tui_model_store_record_recent(const char *path, const char *id, int cap)
{
    if (!path || !id) return -1;
    if (cap < 1) cap = 1;
    char **recent = NULL;
    char **fav = NULL;
    int rc = -1;
    int rc_count = 0, fc_count = 0;
    if (tui_model_store_load(path, &recent, &rc_count, &fav, &fc_count) != 0)
        return -1;

    /* build the new recent list: id first, then others (dedup) */
    int total = rc_count + 1;
    int n = 0;
    char **nr = calloc((size_t)total, sizeof(char *));
    if (!nr) goto done;
    nr[n++] = str_dup(id);
    if (!nr[0]) goto done;
    for (int i = 0; i < rc_count && n < cap; i++)
    {
        if (strcmp(recent[i], id) == 0) continue;
        nr[n] = str_dup(recent[i]);
        if (!nr[n]) goto done;
        n++;
    }
    rc = store_write(path, (const char *const *)nr, n,
                     (const char *const *)fav, fc_count);
done:
    free_list(nr, n);
    free_list(recent, rc_count);
    free_list(fav, fc_count);
    return rc;
}

int tui_model_store_toggle_favorite(const char *path, const char *id,
                                    int *now_favorite)
{
    if (!path || !id) return -1;
    char **recent = NULL;
    char **fav = NULL;
    int rc_count = 0, fc_count = 0;
    if (tui_model_store_load(path, &recent, &rc_count, &fav, &fc_count) != 0)
        return -1;

    int rc = -1;
    int was = list_contains((const char *const *)fav, fc_count, id);
    int n = 0;
    char **nf = calloc((size_t)fc_count + 1, sizeof(char *));
    if (!nf) goto done;
    if (was)
    {
        for (int i = 0; i < fc_count; i++)
            if (strcmp(fav[i], id) != 0)
            {
                nf[n] = str_dup(fav[i]);
                if (!nf[n]) goto done;
                n++;
            }
    }
    else
    {
        nf[n++] = str_dup(id);
        if (!nf[0]) goto done;
        for (int i = 0; i < fc_count; i++)
        {
            nf[n] = str_dup(fav[i]);
            if (!nf[n]) goto done;
            n++;
        }
    }
    rc = store_write(path, (const char *const *)recent, rc_count,
                     (const char *const *)nf, n);
    if (rc == 0 && now_favorite) *now_favorite = !was;
done:
    free_list(nf, n);
    free_list(recent, rc_count);
    free_list(fav, fc_count);
    return rc;
}