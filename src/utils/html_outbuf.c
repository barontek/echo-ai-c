/*
 * html_outbuf.c - growable output buffer with overflow guards.
 * Depends on: libc (stdlib/string), html_internal.h for OutBuf.
 */

#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "html_outbuf.h"
#include "html_internal.h"

#ifdef HTML_EXTRACT_TEST
/* Fault-injection shims (counters live in html_extract.c): route
 * this TU's allocations through the shared hooks so
 * html_extract_test_set_alloc_fail() reaches the whole module. */
void *test_realloc(void *ptr, size_t size);
void *test_malloc(size_t size);
#define malloc test_malloc
#define realloc test_realloc
#endif


int outbuf_reserve(OutBuf *b, size_t extra)
{
    if (extra > SIZE_MAX - b->len - 1) return -1;
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 0;
    size_t new_cap = b->cap ? b->cap * 2 : 64;
    while (new_cap < need)
    {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = need;
            break;
        }
        new_cap *= 2;
    }
    char *new = realloc(b->data, new_cap);
    if (!new) return -1;
    b->data = new;
    b->cap = new_cap;
    return 0;
}

int outbuf_append(OutBuf *b, const char *s, size_t n)
{
    if (outbuf_reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int outbuf_append_chr(OutBuf *b, char c)
{
    if (outbuf_reserve(b, 1) != 0) return -1;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
    return 0;
}

void outbuf_truncate(OutBuf *b, size_t len)
{
    if (len >= b->len) return;
    b->len = len;
    b->data[len] = '\0';
}
