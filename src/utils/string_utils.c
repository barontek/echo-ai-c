/*
 * string_utils.c - common string helpers: in-place trim, duplication,
 * prefix/suffix tests, splitting into fresh copies, JSON sanitizing
 * (markdown fences, stray whitespace, trailing commas), and
 * ellipsis-marked truncation. Depends on: libc only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "string_utils.h"

char *str_trim(char *str)
{
    if (!str) return NULL;

    char *start = str;
    while (isspace((unsigned char)*start)) start++; // NOLINT(clang-analyzer-core.uninitialized.ArraySubscript)

    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';

    if (start != str) memmove(str, start, end - start + 1);
    return str;
}

char *str_dup(const char *str)
{
    if (!str) return NULL;
    size_t len = strlen(str);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, str, len + 1);
    return copy;
}

int str_append(char **target, const char *value)
{
    if (!target || !value) return -1;
    size_t old_length = *target ? strlen(*target) : 0U;
    size_t add_length = strlen(value);
    if (add_length > SIZE_MAX - old_length - 1U) return -1;
    char *grown = realloc(*target, old_length + add_length + 1U);
    if (!grown) return -1;
    memcpy(grown + old_length, value, add_length + 1U);
    *target = grown;
    return 0;
}

/* strlcpy/strlcat are already provided by Apple libSystem (under
 * _FORTIFY_SOURCE they become __builtin___strlcat_chk macros, so defining
 * them here would collide); glibc lacks them, so define them on non-Apple
 * platforms only. Semantics are identical either way. */
#if !defined(__APPLE__)
size_t strlcpy(char *dst, const char *src, size_t size)
{
    if (size == 0) return strlen(src);
    size_t srclen = strlen(src);
    size_t copy = srclen < size - 1 ? srclen : size - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
    return srclen;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dstlen = strlen(dst);
    if (dstlen >= size) return size + strlen(src);
    size_t srclen = strlen(src);
    size_t copy = srclen < size - dstlen - 1 ? srclen : size - dstlen - 1;
    memcpy(dst + dstlen, src, copy);
    dst[dstlen + copy] = '\0';
    return dstlen + srclen;
}
#endif

int str_starts_with(const char *str, const char *prefix)
{
    if (!str || !prefix) return 0;
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

int str_ends_with(const char *str, const char *suffix)
{
    if (!str || !suffix) return 0;
    size_t slen = strlen(str);
    size_t suflen = strlen(suffix);
    if (suflen > slen) return 0;
    return strcmp(str + slen - suflen, suffix) == 0;
}

StrArray str_split(const char *str, char delimiter)
{
    StrArray result = {NULL, 0};
    if (!str) return result;

    int capacity = 8;
    result.items = malloc(sizeof(char *) * capacity);
    if (!result.items) return result;

    const char *start = str;
    const char *p = str;

    while (*p)
    {
        if (*p == delimiter)
        {
            if (result.count >= capacity)
            {
                capacity *= 2;
                char **new_items = realloc(result.items, sizeof(char *) * capacity);
                if (!new_items) {
                    str_array_free(&result);
                    return result;
                }
                result.items = new_items;
            }

            size_t len = p - start;
            result.items[result.count] = malloc(len + 1);
            if (result.items[result.count])
            {
                memcpy(result.items[result.count], start, len);
                result.items[result.count][len] = '\0';
                result.count++;
            }
            start = p + 1;
        }
        p++;
    }

    if (result.count >= capacity)
    {
        capacity *= 2;
        char **new_items = realloc(result.items, sizeof(char *) * capacity);
        if (!new_items) {
            str_array_free(&result);
            return result;
        }
        result.items = new_items;
    }

    result.items[result.count] = str_dup(start);
    if (result.items[result.count]) result.count++;

    return result;
}

void str_array_free(StrArray *arr)
{
    if (!arr || !arr->items) return;
    for (int i = 0; i < arr->count; i++) free(arr->items[i]);
    free(arr->items);
    arr->items = NULL;
    arr->count = 0;
}

char *str_truncate_ellipsis_dup(const char *text, size_t max_chars)
{
    if (!text) return NULL;

    size_t len = strlen(text);
    if (len <= max_chars) return str_dup(text);

    char buf[64];
    int ml = snprintf(buf, sizeof(buf),
                      "[... truncated, %zu chars omitted ...]", len - max_chars);
    if (ml < 0) ml = 0;
    if ((size_t)ml >= sizeof(buf)) ml = (int)sizeof(buf) - 1;
    size_t mlen = (size_t)ml;

    /* Keep the first `keep` chars of text; the marker replaces the tail. */
    size_t keep;
    if (max_chars > mlen + 1)
    {
        keep = max_chars - mlen; /* text + marker == max_chars */
    }
    else
    {
        keep = max_chars; /* no room for the marker; hard cut */
        mlen = 0;
    }
    /* Back off the cut point while the byte at the cut is a UTF-8
     * continuation byte: splitting a multi-byte sequence here would leave
     * invalid UTF-8 behind, and the results of this function go into
     * WebSocket text frames (which must be valid UTF-8) and LLM messages.
     * text[keep] is the first dropped byte; text is NUL-terminated so the
     * read is in-bounds even when keep == len. */
    while (keep > 0 && ((unsigned char)text[keep] & 0xC0) == 0x80)
        keep--;

    char *out = malloc(keep + mlen + 1);
    if (!out) return NULL;
    memcpy(out, text, keep);
    if (mlen) memcpy(out + keep, buf, mlen);
    out[keep + mlen] = '\0';
    return out;
}

char *sanitize_json_dup(const char *str)
{
    if (!str) return NULL;

    const char *p = str;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    if (strncmp(p, "```", 3) == 0)
    {
        p += 3;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    size_t len = strlen(p);
    char *result = malloc(len + 1);
    if (!result) return NULL;

    size_t w = 0;
    int in_string = 0;

    for (size_t i = 0; i < len; i++)
    {
        char c = p[i];

        if (c == '"' && (i == 0 || p[i - 1] != '\\')) in_string = !in_string;

        if (!in_string)
        {
            if (c == '\n' || c == '\r' || c == '\t') continue;
        }

        if (!in_string && c == ',')
        {
            size_t j = i + 1;
            while (j < len && (p[j] == ' ' || p[j] == '\t' || p[j] == '\n' || p[j] == '\r'))
                j++;
            if (j < len && (p[j] == ']' || p[j] == '}'))
            {
                result[w++] = c;
                while (w > 0 && result[w - 1] == ',') w--;
                memcpy(result + w, p + j, len - j);
                result[w + (len - j)] = '\0';
                if (w > 0 && w + (len - j) < len)
                {
                    size_t remaining = len - j;
                    memcpy(result + w, p + j, remaining);
                    w += remaining;
                    result[w] = '\0';
                }
                break;
            }
        }

        result[w++] = c;
    }

    result[w] = '\0';

    size_t end = w;
    while (end > 0 && (result[end - 1] == ' ' || result[end - 1] == '\t' ||
           result[end - 1] == '\n' || result[end - 1] == '\r'))
        end--;
    result[end] = '\0';

    char *trimmed = str_trim(result);
    if (trimmed != result)
    {
        char *final = str_dup(trimmed);
        free(result);
        return final;
    }

    return result;
}
