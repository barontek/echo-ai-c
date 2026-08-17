/*
 * tui_session_store.c - JSON-backed session pins and quick-switch slot
 * resolution. Pins are persisted as {"pinned":[ids]} and read back in
 * order; slot resolution is pure list arithmetic.
 * Depends on: tui_session_store.h, cJSON, string_utils.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "tui_session_store.h"
#include "../utils/string_utils.h"

static char *read_file_alloc(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf)
    {
        fclose(fp);
        return NULL;
    }
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, fp)) > 0)
    {
        len += n;
        if (len == cap)
        {
            size_t nc = cap * 2;
            char *nb = realloc(buf, nc);
            if (!nb)
            {
                free(buf);
                fclose(fp);
                return NULL;
            }
            buf = nb;
            cap = nc;
        }
    }
    fclose(fp);
    buf[len] = '\0';
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

int tui_session_store_load_pins(const char *path, char ***out, int *out_count)
{
    if (!path || !out || !out_count) return -1;
    /* caller frees the empty array even when nothing is pinned */
    *out = calloc(1, sizeof(char *));
    if (!*out) return -1;
    *out_count = 0;
    char *data = read_file_alloc(path);
    if (!data) return 0;

    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root) return 0;
    cJSON *pinned = cJSON_GetObjectItem(root, "pinned");
    int count = pinned && cJSON_IsArray(pinned) ? cJSON_GetArraySize(pinned) : 0;
    char **arr = calloc((size_t)(count > 0 ? count : 1), sizeof(char *));
    if (!arr)
    {
        free(*out);
        *out = NULL;
        cJSON_Delete(root);
        return -1;
    }
    int n = 0;
    if (count > 0)
    {
        cJSON *item;
        cJSON_ArrayForEach(item, pinned)
        {
            const char *s = cJSON_GetStringValue(item);
            if (!s) continue;
            arr[n] = str_dup(s);
            if (!arr[n])
            {
                for (int i = 0; i < n; i++) free(arr[i]);
                free(arr);
                free(*out);
                *out = NULL;
                cJSON_Delete(root);
                return -1;
            }
            n++;
        }
    }
    cJSON_Delete(root);
    free(*out);
    *out = arr;
    *out_count = n;
    return 0;
}

int tui_session_store_save_pins(const char *path, const char *const *ids,
                                int count)
{
    if (!path) return -1;
    if (count < 0) count = 0;
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON *arr = cJSON_CreateArray();
    if (!arr)
    {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddItemToObject(root, "pinned", arr);
    for (int i = 0; i < count; i++)
    {
        if (ids && ids[i])
            cJSON_AddItemToArray(arr, cJSON_CreateString(ids[i]));
    }
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

int tui_session_store_toggle_pin(const char *path, const char *id,
                                 int *now_pinned)
{
    if (!path || !id) return -1;
    char **pins = NULL;
    int count = 0;
    if (tui_session_store_load_pins(path, &pins, &count) != 0) return -1;

    int idx = -1;
    for (int i = 0; i < count; i++)
        if (strcmp(pins[i], id) == 0) { idx = i; break; }

    int rc;
    if (idx >= 0)
    {
        /* unpin: remove it */
        free(pins[idx]);
        memmove(pins + idx, pins + idx + 1, (size_t)(count - idx - 1) * sizeof(char *));
        count--;
        rc = tui_session_store_save_pins(path, (const char *const *)pins, count);
        for (int i = 0; i < count; i++) free(pins[i]);
        free(pins);
        if (rc == 0 && now_pinned) *now_pinned = 0;
        return rc;
    }

    /* pin: insert at the front (most recent pin) */
    char **nb = realloc(pins, (size_t)(count + 1) * sizeof(char *));
    if (!nb)
    {
        for (int i = 0; i < count; i++) free(pins[i]);
        free(pins);
        return -1;
    }
    pins = nb;
    char *copy = str_dup(id);
    if (!copy)
    {
        for (int i = 0; i < count; i++) free(pins[i]);
        free(pins);
        return -1;
    }
    memmove(pins + 1, pins, (size_t)count * sizeof(char *));
    pins[0] = copy;
    count++;
    rc = tui_session_store_save_pins(path, (const char *const *)pins, count);
    for (int i = 0; i < count; i++) free(pins[i]);
    free(pins);
    if (rc == 0 && now_pinned) *now_pinned = 1;
    return rc;
}

int tui_session_store_resolve_slot(const char *const *pinned, int pinned_count,
                                   const char *const *all, int all_count,
                                   int slot, char *out, size_t out_cap)
{
    if (!out || out_cap < 1) return 0;
    if (slot < 1) return 0;

    /* pass 1: pinned ids (that still exist) */
    int seen = 0;
    for (int i = 0; i < pinned_count; i++)
    {
        if (!pinned[i]) continue;
        if (!list_contains(all, all_count, pinned[i])) continue;
        seen++;
        if (seen == slot)
        {
            snprintf(out, out_cap, "%s", pinned[i]);
            return 1;
        }
    }
    /* pass 2: every non-pinned session in order */
    for (int i = 0; i < all_count; i++)
    {
        if (!all[i]) continue;
        if (list_contains(pinned, pinned_count, all[i])) continue;
        seen++;
        if (seen == slot)
        {
            snprintf(out, out_cap, "%s", all[i]);
            return 1;
        }
    }
    return 0;
}