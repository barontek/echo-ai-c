/*
 * html_extract.c - readable-text extraction from raw HTML: parse
 * loop, frame keep/drop, citation footer, head-biased truncation,
 * and Content-Type dispatch. The encoding, entity, tag, buffer, and
 * writer stages live in html_unicode/entities/tags/outbuf/writer
 * units; shared state in html_internal.h.
 * Depends on: string_utils.h, libc (ctype/math/stdio/stdlib).
 */

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html_extract.h"
#include "html_internal.h"
#include "html_unicode.h"
#include "html_entities.h"
#include "html_tags.h"
#include "html_outbuf.h"
#include "html_writer.h"
#include "html_assembly.h"
#include "string_utils.h"

/* Fault injection for allocation-failure tests (AGENTS.md section 11):
 * only translation units compiled with HTML_EXTRACT_TEST see the
 * shims, so production builds are unaffected. */
#ifdef HTML_EXTRACT_TEST
static int extract_alloc_counter = 0;
static int extract_alloc_fail_at = -1;

void html_extract_test_set_alloc_fail(int nth_allocation)
{
    extract_alloc_counter = 0;
    extract_alloc_fail_at = nth_allocation;
}

void *test_realloc(void *ptr, size_t size)
{
    extract_alloc_counter++;
    if (extract_alloc_counter == extract_alloc_fail_at) return NULL;
    return realloc(ptr, size);
}

void *test_malloc(size_t size)
{
    extract_alloc_counter++;
    if (extract_alloc_counter == extract_alloc_fail_at) return NULL;
    return malloc(size);
}

#define malloc test_malloc
#define realloc test_realloc
#endif



static int emit_text_run(Writer *w, const char *s, size_t n, int is_pre)
{
    if (n == 0) return 0;
    if (utf8_valid(s, n))
    {
        if (is_pre)
        {
            if (outbuf_append(&w->out, s, n) != 0) return -1;
            w->has_last = 1;
            w->last = s[n - 1];
        }
        else
        {
            if (writer_text(w, s, n) != 0) return -1;
        }
        return 0;
    }
    char buf[4];
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)s[i];
        size_t blen;
        if (c < 0x80)
        {
            buf[0] = (char)c;
            blen = 1;
        }
        else
        {
            blen = utf8_encode_cp(cp1252_cp(c), buf);
        }
        if (blen == 0)
        {
            buf[0] = (char)0xEF; buf[1] = (char)0xBF;
            buf[2] = (char)0xBD; blen = 3; /* U+FFFD: undecodable byte */
        }
        if (is_pre)
        {
            if (outbuf_append(&w->out, buf, blen) != 0) return -1;
            w->has_last = 1;
            w->last = buf[blen - 1];
        }
        else
        {
            if (writer_text(w, buf, blen) != 0) return -1;
        }
    }
    return 0;
}

static int emit_text(Extract *x, const char *s, size_t n)
{
    if (n == 0) return 0;
    Frame *top = &x->frames[x->depth - 1];

    if (x->title_active)
        return append_title_text(&x->title, s, n);

    top->word_count += count_words(s, n);
    int is_pre = top->is_pre;
    if (is_pre && writer_preflush(&x->w) != 0) return -1;

    size_t i = 0;
    while (i < n)
    {
        size_t run_start = i;
        while (i < n && s[i] != '&' && s[i] != '<') i++;
        size_t run_len = i - run_start;
        if (run_len > 0)
        {
            if (emit_text_run(&x->w, s + run_start, run_len, is_pre) != 0)
                return -1;
            top->text_len += run_len;
            if (x->link_active)
            {
                top->link_text_len += run_len;
                if (run_has_nonws(s + run_start, run_len)) x->link_has_text = 1;
            }
        }
        if (i >= n) break;
        if (s[i] == '&')
        {
            char ebuf[4];
            size_t elen = 0;
            size_t consumed = 0;
            if (decode_entity(s + i, n - i, ebuf, &elen, &consumed))
            {
                if (emit_text_run(&x->w, ebuf, elen, is_pre) != 0) return -1;
                top->text_len += 1;
                if (x->link_active)
                {
                    top->link_text_len += 1;
                    int all_ws = 1;
                    for (size_t k = 0; k < elen; k++)
                    {
                        if (!isspace((unsigned char)ebuf[k])) {
                            all_ws = 0;
                            break;
                        }
                    }
                    if (!all_ws) x->link_has_text = 1;
                }
                i += consumed;
                continue;
            }
            /* not an entity: emit the '&' literally */
            if (emit_text_run(&x->w, "&", 1, is_pre) != 0) return -1;
            top->text_len += 1;
            if (x->link_active)
            {
                top->link_text_len += 1;
                x->link_has_text = 1;
            }
            i++;
            continue;
        }
        i++; /* stray '<' inside a text run: keep scanning */
    }
    return 0;
}

static int frame_close(Extract *x, size_t pos)
{
    if (x->depth <= 1) return 0; /* root never closes */
    Frame *f = &x->frames[x->depth - 1];
    f->end_pos = pos;
    Frame *parent = &x->frames[x->depth - 2];
    parent->text_len += f->text_len;
    parent->link_text_len += f->link_text_len;
    parent->word_count += f->word_count;
    x->depth--;

    size_t min_words = f->is_header ? 0 : MIN_CONTENT_WORDS;
    if (frame_score(f) < SCORE_THRESHOLD || f->word_count < min_words)
    {
        outbuf_truncate(&x->w.out, f->out_start);
        writer_sync_state(&x->w);
        /* Links collected inside a dropped frame must not appear in the
         * footer (their text was rolled back, so citations would dangle). */
        for (int i = f->link_at_open; i < x->link_count; i++)
            free(x->links[i].url);
        x->link_count = f->link_at_open;
    }
    return 0;
}

static int link_resolve_index(Extract *x, const char *url, size_t len)
{
    if (len == 0) return -1;
    if (url[0] == '#') return -1;
    if (len >= 11 && memcmp(url, "javascript:", 11) == 0) return -1;
    for (int i = 0; i < x->link_count; i++)
    {
        if (strlen(x->links[i].url) == len &&
            memcmp(x->links[i].url, url, len) == 0)
            return i;
    }
    if (x->link_count >= MAX_LINKS) return -1;
    char *u = malloc(len + 1);
    if (!u) return -1;
    memcpy(u, url, len);
    u[len] = '\0';
    x->links[x->link_count].url = u;
    int idx = x->link_count;
    x->link_count++;
    return idx;
}

static size_t find_seq(const char *s, size_t len, size_t start,
                       const char *pat, size_t plen)
{
    if (plen > len || start > len) return len;
    for (size_t i = start; i + plen <= len; i++)
    {
        size_t j = 0;
        while (j < plen && s[i + j] == pat[j]) j++;
        if (j == plen) return i;
    }
    return len;
}

static size_t find_tag_end(const char *s, size_t len, size_t start)
{
    char quote = 0;
    for (size_t i = start; i < len; i++)
    {
        char c = s[i];
        if (quote)
        {
            if (c == quote) quote = 0;
        }
        else if (c == '"' || c == '\'')
        {
            quote = c;
        }
        else if (c == '>')
        {
            return i;
        }
    }
    return len;
}

static void copy_attr_val(char *dst, size_t cap, size_t *dlen,
                          const char *val, size_t vlen)
{
    if (vlen >= cap) vlen = cap - 1;
    memcpy(dst, val, vlen);
    dst[vlen] = '\0';
    *dlen = vlen;
}

static int ieq(const char *a, size_t alen, const char *b)
{
    size_t blen = strlen(b);
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++)
    {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

static void scan_attrs(const char *s, size_t n,
                       char *href, size_t href_cap, size_t *href_len,
                       char *cls, size_t cls_cap, size_t *cls_len,
                       char *idv, size_t idv_cap, size_t *idv_len)
{
    size_t i = 0;
    while (i < n && !isspace((unsigned char)s[i]) && s[i] != '/') i++;
    while (i < n)
    {
        while (i < n && (isspace((unsigned char)s[i]) || s[i] == '/')) i++;
        if (i >= n) break;
        size_t aname = i;
        while (i < n && !isspace((unsigned char)s[i]) &&
               s[i] != '=' && s[i] != '/')
            i++;
        size_t aname_len = i - aname;
        while (i < n && isspace((unsigned char)s[i])) i++;
        if (i < n && s[i] == '=')
        {
            i++;
            while (i < n && isspace((unsigned char)s[i])) i++;
            char quote = 0;
            if (i < n && (s[i] == '"' || s[i] == '\''))
            {
                quote = s[i];
                i++;
            }
            size_t vstart = i;
            if (quote)
            {
                while (i < n && s[i] != quote) i++;
            }
            else
            {
                while (i < n && !isspace((unsigned char)s[i]) && s[i] != '/') i++;
            }
            size_t vlen = i - vstart;
            const char *val = s + vstart;
            if (href && ieq(s + aname, aname_len, "href"))
                copy_attr_val(href, href_cap, href_len, val, vlen);
            else if (cls && ieq(s + aname, aname_len, "class"))
                copy_attr_val(cls, cls_cap, cls_len, val, vlen);
            else if (idv && ieq(s + aname, aname_len, "id"))
                copy_attr_val(idv, idv_cap, idv_len, val, vlen);
            if (quote && i < n && s[i] == quote) i++;
        }
        /* attribute without a value: nothing to extract */
    }
}

static int handle_open_tag(Extract *x, size_t tag_start, size_t tag_end)
{
    const char *s = x->raw + tag_start + 1;
    size_t n = tag_end - tag_start - 1;
    size_t name_len = 0;
    while (name_len < n && !isspace((unsigned char)s[name_len]) &&
           s[name_len] != '/' && s[name_len] != '>')
        name_len++;
    TagId tid = tag_lookup(s, name_len);
    int self_closing = n > 0 && s[n - 1] == '/';

    if (is_excluded_tag(tid))
    {
        x->skip_depth++;
        if (tid == TAG_EXCL_SCRIPT || tid == TAG_EXCL_STYLE)
        {
            /* raw-text: remember the element so the parse loop can skip to
             * its close tag without misparsing '<' inside its content */
            if (name_len < sizeof(x->raw_tag))
            {
                memcpy(x->raw_tag, s, name_len);
                x->raw_tag[name_len] = '\0';
                x->raw_tag_len = name_len;
            }
        }
        return 0;
    }
    if (tid == TAG_SPECIAL_TITLE)
    {
        if (x->skip_depth == 0) x->title_active = 1;
        return 0;
    }
    if (x->skip_depth > 0) return 0; /* inside excluded content: ignore */

    if (tid == TAG_VOID_NL_BR || tid == TAG_VOID_NL_HR)
    {
        writer_newline(&x->w);
        return 0;
    }
    if (tid == TAG_SPECIAL_A)
    {
        if (self_closing) return 0;
        x->link_url_len = 0;
        scan_attrs(s, n, x->link_url, MAX_HREF_LEN, &x->link_url_len,
                   NULL, 0, NULL, NULL, 0, NULL);
        x->link_active = 1;
        x->link_has_text = 0;
        return 0;
    }
    if (is_frame_tag(tid))
    {
        if (self_closing) return 0;
        if (x->depth >= MAX_FRAME_DEPTH) return 0; /* too deep: transparent */
        char cls[MAX_ATTR_LEN] = {0};
        size_t clen = 0;
        char idv[MAX_ATTR_LEN] = {0};
        size_t ilen = 0;
        scan_attrs(s, n, NULL, 0, NULL, cls, MAX_ATTR_LEN, &clen,
                   idv, MAX_ATTR_LEN, &ilen);
        Frame *f = &x->frames[x->depth];
        memset(f, 0, sizeof(*f));
        f->tag_id = tid;
        f->start_pos = tag_end + 1;
        f->out_start = x->w.out.len;
        f->link_at_open = x->link_count;
        f->is_pre = (tid == TAG_FRAME_PRE);
        f->is_header = is_header_tag(tid);
        f->class_neg = has_neg_pattern(cls, clen) || has_neg_pattern(idv, ilen);
        x->depth++;
        writer_newline(&x->w);
        return 0;
    }
    return 0; /* transparent (inline/unknown tags) */
}

static int handle_close_tag(Extract *x, size_t tag_start, size_t tag_end)
{
    const char *s = x->raw + tag_start + 2;
    size_t n = tag_end - tag_start - 2;
    size_t name_len = 0;
    while (name_len < n && !isspace((unsigned char)s[name_len]) &&
           s[name_len] != '>')
        name_len++;
    TagId tid = tag_lookup(s, name_len);

    if (tid == TAG_SPECIAL_A)
    {
        if (!x->link_active) return 0;
        x->link_active = 0;
        if (!x->link_has_text) return 0;
        int idx = link_resolve_index(x, x->link_url, x->link_url_len);
        if (idx < 0) return 0;
        char mk[16];
        int mlen = snprintf(mk, sizeof(mk), " [%d]", idx + 1);
        if (mlen <= 0 || (size_t)mlen >= sizeof(mk)) return 0;
        return writer_text(&x->w, mk, (size_t)mlen);
    }
    if (tid == TAG_SPECIAL_TITLE)
    {
        x->title_active = 0;
        return 0;
    }
    if (is_excluded_tag(tid))
    {
        if (x->skip_depth > 0) x->skip_depth--;
        return 0;
    }
    if (is_frame_tag(tid))
    {
        /* find the matching open frame, auto-closing misnested ones above */
        int d = x->depth - 1;
        while (d > 0 && x->frames[d].tag_id != tid) d--;
        if (d <= 0) return 0;
        while (x->depth - 1 > d)
        {
            if (frame_close(x, tag_start) != 0) return -1;
        }
        return frame_close(x, tag_start);
    }
    return 0;
}

static int parse(Extract *x)
{
    size_t i = 0;
    while (i < x->len)
    {
        if (x->raw[i] == '<' && x->skip_depth > 0 && x->raw_tag_len > 0)
        {
            /* raw-text skip (script/style): jump to the literal close tag;
             * content may contain '<' that would confuse the tag scanner */
            size_t k = x->len;
            for (size_t j = i; j + 2 + x->raw_tag_len <= x->len; j++)
            {
                if (x->raw[j] == '<' && x->raw[j + 1] == '/' &&
                    memcmp(x->raw + j + 2, x->raw_tag, x->raw_tag_len) == 0)
                {
                    k = j;
                    break;
                }
            }
            if (k == x->len)
            {
                i = x->len;
                x->skip_depth = 0;
                x->raw_tag_len = 0;
                continue;
            }
            size_t te = find_tag_end(x->raw, x->len, k + 1);
            i = (te == x->len) ? x->len : te + 1;
            x->skip_depth--;
            x->raw_tag_len = 0;
            continue;
        }
        if (x->raw[i] != '<')
        {
            size_t j = i;
            while (j < x->len && x->raw[j] != '<') j++;
            if (x->skip_depth == 0 && emit_text(x, x->raw + i, j - i) != 0)
                return -1;
            i = j;
            continue;
        }

        if (i + 1 < x->len && x->raw[i + 1] == '!')
        {
            size_t k;
            if (i + 3 < x->len && x->raw[i + 2] == '-' && x->raw[i + 3] == '-')
            {
                /* <!-- comment --> */
                k = find_seq(x->raw, x->len, i + 4, "-->", 3);
                i = (k != x->len) ? k + 3 : x->len;
            }
            else if (i + 8 < x->len &&
                     strncmp(x->raw + i + 2, "[CDATA[", 7) == 0)
            {
                k = find_seq(x->raw, x->len, i + 9, "]]>", 3);
                i = (k != x->len) ? k + 3 : x->len;
            }
            else
            {
                /* doctype or other declaration */
                k = find_seq(x->raw, x->len, i + 2, ">", 1);
                i = (k != x->len) ? k + 1 : x->len;
            }
            continue;
        }

        if (i + 1 < x->len && x->raw[i + 1] == '?')
        {
            size_t k = find_seq(x->raw, x->len, i + 2, ">", 1);
            i = (k != x->len) ? k + 1 : x->len;
            continue;
        }

        size_t te = find_tag_end(x->raw, x->len, i + 1);
        if (te == x->len)
        {
            /* stray '<' with no tag terminator: treat as text */
            if (x->skip_depth == 0 && emit_text(x, x->raw + i, 1) != 0)
                return -1;
            i++;
            continue;
        }

        int rc = (te > i + 1 && x->raw[i + 1] == '/')
            ? handle_close_tag(x, i, te)
            : handle_open_tag(x, i, te);
        if (rc != 0) return -1;
        i = te + 1;
    }

    while (x->depth > 1)
    {
        if (frame_close(x, x->len) != 0) return -1;
    }
    return 0;
}

char *html_extract_text_alloc(const char *raw, size_t raw_len, size_t max_chars)
{
    if (!raw) return str_dup("");
    if (max_chars == 0) return str_dup("");

    Extract x;
    memset(&x, 0, sizeof(x));
    x.raw = raw;
    x.len = raw_len;
    x.max_chars = max_chars;
    x.frames[0].tag_id = -1; /* root frame: never scored, never dropped */
    x.depth = 1;

    if (parse(&x) != 0)
    {
        free(x.w.out.data);
        free(x.title.data);
        for (int i = 0; i < x.link_count; i++) free(x.links[i].url);
        return NULL;
    }

    char *res = assemble(&x);
    free(x.w.out.data);
    free(x.title.data);
    for (int i = 0; i < x.link_count; i++) free(x.links[i].url);
    return res;
}

static char *text_for_llm(const char *data, size_t len, size_t max_chars)
{
    char *nul = malloc(len + 1);
    if (!nul) return NULL;
    memcpy(nul, data, len);
    nul[len] = '\0';
    if (!utf8_valid(nul, len))
    {
        /* Plain-text responses need not be UTF-8 (latin-1/CP1252 is
         * common); transcode so the result is always decodable — invalid
         * bytes in a tool result break the frontend WebSocket and strict
         * LLM providers. Transcoded output is at most 2x, so allocate for
         * the worst case and copy back. */
        char *fixed = NULL;
        if (len <= SIZE_MAX / 2)
            fixed = malloc(len * 2 + 1);
        if (fixed)
        {
            size_t w = 0;
            char buf[4];
            for (size_t i = 0; i < len; i++)
            {
                unsigned char c = (unsigned char)nul[i];
                size_t blen;
                if (c < 0x80)
                {
                    buf[0] = (char)c;
                    blen = 1;
                }
                else
                {
                    blen = utf8_encode_cp(cp1252_cp(c), buf);
                }
                if (blen == 0)
                {
                    buf[0] = (char)0xEF; buf[1] = (char)0xBF;
                    buf[2] = (char)0xBD; blen = 3; /* U+FFFD */
                }
                memcpy(fixed + w, buf, blen);
                w += blen;
            }
            fixed[w] = '\0';
            free(nul);
            nul = fixed;
            len = w;
        }
    }
    char *res = str_truncate_ellipsis_dup(nul, max_chars);
    free(nul);
    return res;
}

char *content_extract_for_llm_alloc(const char *content_type, const char *data,
                              size_t len, size_t max_chars)
{
    if (!data || len == 0) return str_dup("(empty response)");
    if (max_chars == 0) return str_dup("");

    if (!content_type || !content_type[0])
    {
        /* no Content-Type header: sniff the first non-space byte */
        size_t i = 0;
        while (i < len && isspace((unsigned char)data[i])) i++;
        if (i < len && data[i] == '<') return html_extract_text_alloc(data, len, max_chars);
        return text_for_llm(data, len, max_chars);
    }

    if (prefix_ieq(content_type, "text/html") ||
        prefix_ieq(content_type, "application/xhtml+xml"))
        return html_extract_text_alloc(data, len, max_chars);

    if (prefix_ieq(content_type, "text/") ||
        prefix_ieq(content_type, "application/json") ||
        prefix_ieq(content_type, "application/xml") ||
        prefix_ieq(content_type, "application/javascript"))
        return text_for_llm(data, len, max_chars);

    /* binary: never dump raw bytes into the LLM context */
    size_t ctlen = strlen(content_type);
    if (ctlen > 512) ctlen = 512;
    size_t cap = 160 + ctlen + 32;
    char *res = malloc(cap);
    if (!res) return NULL;
    int n = snprintf(res, cap,
                     "Binary content (%.*s, %zu bytes). Not shown; "
                     "use a file tool to download it if needed.",
                     (int)ctlen, content_type, len);
    if (n < 0 || (size_t)n >= cap)
    {
        free(res);
        return str_dup("Binary content (unrecognized type).");
    }
    return res;
}
