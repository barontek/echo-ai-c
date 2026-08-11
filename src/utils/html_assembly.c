/*
 * html_assembly.c - final output assembly: title prefix, body,
 * truncation marker (with paragraph/word/UTF-8 boundary backing),
 * and the citation footer.
 * Depends on: html_outbuf, html_unicode, html_internal.h.
 */

#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html_assembly.h"
#include "html_internal.h"
#include "html_outbuf.h"
#include "html_unicode.h"

#ifdef HTML_EXTRACT_TEST
/* Fault-injection shims (counters live in html_extract.c): route
 * this TU's allocations through the shared hooks so
 * html_extract_test_set_alloc_fail() reaches the whole module. */
void *test_realloc(void *ptr, size_t size);
void *test_malloc(size_t size);
#define malloc test_malloc
#define realloc test_realloc
#endif


static int build_footer(Extract *x, OutBuf *fb)
{
    if (x->link_count == 0) return 0;
    if (outbuf_append(fb, "\n\nLinks:\n", 9) != 0) return -1;
    for (int i = 0; i < x->link_count; i++)
    {
        char num[16];
        int nl = snprintf(num, sizeof(num), "%d. ", i + 1);
        if (nl <= 0 || (size_t)nl >= sizeof(num)) continue;
        if (outbuf_append(fb, num, (size_t)nl) != 0) return -1;
        if (outbuf_append(fb, x->links[i].url, strlen(x->links[i].url)) != 0)
            return -1;
        if (outbuf_append_chr(fb, '\n') != 0) return -1;
    }
    return 0;
}

static size_t assemble_copy_parts(char *dst, const char *title, size_t tlen,
                                  const char *body, size_t body_len,
                                  const char *footer, size_t flen,
                                  const char *marker, size_t mlen)
{
    size_t w = 0;
    if (title)
    {
        const char *tprefix = "Title: ";
        size_t plen = 7;
        memcpy(dst + w, tprefix, plen); w += plen;
        memcpy(dst + w, title, tlen); w += tlen;
        memcpy(dst + w, "\n\n", 2);   w += 2;
    }
    if (body_len > 0)
    {
        memcpy(dst + w, body, body_len);
        w += body_len;
    }
    if (mlen > 0) {
        memcpy(dst + w, marker, mlen);
        w += mlen;
    }
    if (flen > 0) {
        memcpy(dst + w, footer, flen);
        w += flen;
    }
    return w;
}

static size_t assemble_cut(const OutBuf *out, size_t budget, size_t *omitted)
{
    size_t body_len = out->len;
    /* Case 2 guarantees body_len > budget (else total would have fit), but
     * keep the guard so the boundary walk can never shrink a body that
     * already fits the budget. */
    if (body_len <= budget)
    {
        *omitted = 0;
        return body_len;
    }
    size_t cut = budget;
    size_t k = cut;
    while (k > 0 && out->data[k - 1] != '\n') k--;
    if (k > 0 && k != cut)
    {
        cut = k; /* paragraph boundary */
    }
    else if (k == 0)
    {
        k = cut;
        while (k > 0 && out->data[k - 1] != ' ') k--;
        if (k > 0) cut = k; /* word boundary fallback */
    }
    cut = utf8_cut_boundary(out->data, cut);
    *omitted = body_len - cut;
    return cut;
}

char *assemble(Extract *x)
{
    size_t max_chars = x->max_chars;
    char *title = x->title.len > 0 ? x->title.data : NULL;
    size_t tlen = title ? x->title.len + 7 : 0; /* "Title: " prefix + text */
    size_t body_len = x->w.out.len;

    OutBuf footer = {0};
    if (build_footer(x, &footer) != 0) {
        free(footer.data);
        return NULL;
    }
    size_t flen = footer.len;

    size_t total = body_len + tlen + flen + (tlen > 0 ? 2 : 0);
    if (total <= max_chars)
    {
        char *res = malloc(total + 1);
        if (!res) {
            free(footer.data);
            return NULL;
        }
        size_t w = assemble_copy_parts(res, title, x->title.len,
                                       x->w.out.data, body_len,
                                       footer.data, flen, NULL, 0);
        res[w] = '\0';
        free(footer.data);
        return res;
    }

    /* Truncate: drop the footer first, then the title, when the budget is
     * too tight to fit them alongside the truncated body. */
    size_t overhead = tlen + (tlen > 0 ? 2 : 0) + flen;
    if (overhead + MARKER_ROOM > max_chars)
    {
        free(footer.data);
        footer.data = NULL;
        footer.len = 0;
        flen = 0;
        overhead = tlen + (tlen > 0 ? 2 : 0);
    }
    if (overhead + MARKER_ROOM > max_chars)
    {
        title = NULL;
        tlen = 0;
        overhead = 0;
    }

    size_t budget = 0;
    int marker_ok = 0;
    if (max_chars > overhead + MARKER_ROOM)
    {
        budget = max_chars - overhead - MARKER_ROOM;
        marker_ok = 1;
    }
    else if (max_chars > overhead)
    {
        budget = max_chars - overhead;
    }

    size_t omitted = 0;
    size_t cut = assemble_cut(&x->w.out, budget, &omitted);
    if (body_len <= budget) marker_ok = 0;

    char marker[64];
    size_t mlen = 0;
    if (marker_ok && omitted > 0)
    {
        int ml = snprintf(marker, sizeof(marker),
                          "\n[... truncated, %zu chars omitted ...]", omitted);
        if (ml < 0) ml = 0;
        if ((size_t)ml >= sizeof(marker)) ml = (int)sizeof(marker) - 1;
        /* keep the marker only if it actually fits alongside the cut body */
        if (tlen + (tlen > 0 ? 2 : 0) + cut + (size_t)ml + flen <= max_chars)
            mlen = (size_t)ml;
    }

    size_t fin = tlen + (tlen > 0 ? 2 : 0) + cut + mlen + flen;
    char *res = malloc(fin + 1);
    if (!res) {
        free(footer.data);
        return NULL;
    }
    size_t w = assemble_copy_parts(res, title, x->title.len,
                                   x->w.out.data, cut,
                                   footer.data, flen, marker, mlen);
    res[w] = '\0';
    free(footer.data);
    return res;
}
