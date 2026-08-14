/*
 * tool_args.c - compact one-line rendering of tool-call argument JSON.
 * The chat header and the live-activity strip cannot display a raw
 * {"command": "ls -la", ...} blob, so object entries collapse to
 * "key=value" pairs. Degrades to the raw input (truncated) when the
 * input is not JSON or not an object. Depends on: cJSON.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "tool_args.h"

/* Cut the built string to at most max_len bytes at a UTF-8 codepoint
 * boundary, appending "…" only when the marker also fits — the same
 * marker rule the chat header truncator uses, in bytes. @used is the
 * number of appended bytes, so every read in [0, used) is provably
 * initialized (buf_append memcpy'd it); @cap is the buffer capacity
 * (max_len + 8), so the trailing NUL write is provably in bounds.
 * Returns the new length. */
static size_t truncate_with_ellipsis(char *s, size_t used, size_t cap,
                                     size_t max_len)
{
    if (used <= max_len) return used;
    size_t last = 0; /* end of the last codepoint that fit */
    size_t i = 0;
    while (i < used)
    {
        unsigned char c = (unsigned char)s[i];
        size_t n;
        if (c < 0x80) n = 1;
        else if ((c & 0xe0) == 0xc0) n = 2;
        else if ((c & 0xf0) == 0xe0) n = 3;
        else n = 4;
        if (i + n + 1 > max_len) break;
        i += n;
        last = i;
        if (last >= max_len) break;
    }
    if (i < used && max_len - last >= 4)
    {
        s[last] = '\0';
        memcpy(s + last, "\xE2\x80\xA6", 4); /* "…" + NUL */
        return last + 3;
    }
    if (last >= cap) last = cap - 1; /* defensive; loop math keeps last < cap */
    s[last] = '\0';
    return last;
}

/* Append up to cap bytes; a value longer than the room left is cut
 * (possibly mid-codepoint) and truncate_with_ellipsis fixes the tail. */
static void buf_append(char *buf, size_t *used, size_t cap,
                       const char *s, size_t n)
{
    if (n > cap - *used) n = cap - *used;
    memcpy(buf + *used, s, n);
    *used += n;
    buf[*used] = '\0';
}

static void append_pair(char *buf, size_t *used, size_t cap,
                        const char *key, const cJSON *val)
{
    size_t kn = strlen(key);
    buf_append(buf, used, cap, key, kn);
    buf_append(buf, used, cap, "=", 1);
    if (cJSON_IsString(val) && val->valuestring)
        buf_append(buf, used, cap, val->valuestring,
                   strlen(val->valuestring));
    else
    {
        char *flat = cJSON_PrintUnformatted(val);
        if (flat)
        {
            buf_append(buf, used, cap, flat, strlen(flat));
            free(flat);
        }
    }
}

char *tool_args_compact(const char *json, size_t max_len)
{
    if (max_len < 1) max_len = 1;
    /* room for the trailing marker + NUL (the marker is only kept when
     * it fits inside max_len; +1 covers the NUL at a full buffer) */
    size_t cap = max_len + 8;
    char *buf = malloc(cap + 1);
    if (!buf) return NULL;
    buf[0] = '\0';

    size_t used = 0;
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (root && cJSON_IsObject(root))
    {
        cJSON *entry = NULL;
        int first = 1;
        cJSON_ArrayForEach(entry, root)
        {
            if (!entry->string) continue; /* corrupted tree: skip the entry */
            if (!first)
                buf_append(buf, &used, cap, ", ", 2);
            append_pair(buf, &used, cap, entry->string, entry);
            first = 0;
        }
    }
    else if (json)
    {
        /* Degraded: show the raw arguments, bounded to the same budget. */
        buf_append(buf, &used, cap, json, strlen(json));
    }
    if (root) cJSON_Delete(root);

    (void)truncate_with_ellipsis(buf, used, cap, max_len);
    return buf;
}
