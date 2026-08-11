/*
 * html_writer.c - text writer: entity-aware appends with
 * whitespace collapsing, frame scoring, and title rendering.
 * Depends on: html_outbuf, html_unicode, html_entities,
 * html_tags, libc.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "html_writer.h"
#include "html_internal.h"
#include "html_outbuf.h"
#include "html_unicode.h"
#include "html_entities.h"
#include "html_tags.h"


void writer_newline(Writer *w)
{
    w->need_newline = 1;
}

int writer_preflush(Writer *w)
{
    if (w->need_newline)
    {
        /* never start the output with a newline (first frame / rollback) */
        if (w->out.len > 0 &&
            outbuf_append_chr(&w->out, '\n') != 0) return -1;
        w->need_newline = 0;
        w->ws_pending = 0;
        w->has_last = 1;
        w->last = '\n';
    }
    else if (w->ws_pending && w->has_last && w->last != '\n')
    {
        if (outbuf_append_chr(&w->out, ' ') != 0) return -1;
        w->ws_pending = 0;
        w->has_last = 1;
        w->last = ' ';
    }
    return 0;
}

int writer_text(Writer *w, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if (isspace(c))
        {
            /* a pending newline wins over a pending space */
            if (!w->need_newline) w->ws_pending = 1;
            continue;
        }
        if (w->need_newline)
        {
            if (w->out.len > 0 &&
                outbuf_append_chr(&w->out, '\n') != 0) return -1;
            w->need_newline = 0;
            w->ws_pending = 0;
            w->has_last = 1;
            w->last = '\n';
        }
        else if (w->ws_pending && w->has_last && w->last != '\n')
        {
            if (outbuf_append_chr(&w->out, ' ') != 0) return -1;
            w->has_last = 1;
            w->last = ' ';
        }
        w->ws_pending = 0;
        if (outbuf_append_chr(&w->out, (char)c) != 0) return -1;
        w->has_last = 1;
        w->last = (char)c;
    }
    return 0;
}

void writer_sync_state(Writer *w)
{
    if (w->out.len > 0)
    {
        w->last = w->out.data[w->out.len - 1];
        w->has_last = 1;
    }
    else
    {
        w->last = '\0';
        w->has_last = 0;
    }
    w->ws_pending = 0;
}

size_t count_words(const char *s, size_t n)
{
    size_t words = 0;
    int in_word = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (isspace((unsigned char)s[i])) in_word = 0;
        else if (!in_word)
        {
            in_word = 1;
            words++;
        }
    }
    return words;
}

int run_has_nonws(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (!isspace((unsigned char)s[i])) return 1;
    }
    return 0;
}

double frame_score(const Frame *f)
{
    size_t tag_len = f->end_pos - f->start_pos;
    double density = tag_len > 0 ? (double)f->text_len / (double)tag_len : 0.0;
    double link_density = f->text_len > 0
        ? 1.0 - (double)f->link_text_len / (double)f->text_len
        : 0.0;
    if (link_density < 0.0) link_density = 0.0;
    /* Crawl4AI computes a -0.5 class penalty but then clamps it to 0 via
     * max(0, ...), which makes the metric dead code; we keep the penalty so
     * nav/footer/sidebar-classed frames fall below the threshold. */
    double class_score = f->class_neg ? -0.5 : 0.0;
    double text_length = log((double)f->text_len + 1.0);
    /* metric weights: text_density 0.4, link_density 0.2, tag_weight 0.2,
     * class_id 0.1, text_length 0.1 (sum = 1.0) */
    return 0.4 * density + 0.2 * link_density + 0.2 * tag_weight_of(f->tag_id)
         + 0.1 * class_score + 0.1 * text_length;
}

static int title_append_byte(OutBuf *b, unsigned char c, int *ws_pending)
{
    if (isspace(c))
    {
        *ws_pending = 1;
        return 0;
    }
    if (*ws_pending && b->len > 0)
    {
        if (outbuf_append_chr(b, ' ') != 0) return -1;
    }
    *ws_pending = 0;
    return outbuf_append_chr(b, (char)c);
}

int append_title_text(OutBuf *b, const char *s, size_t n)
{
    size_t i = 0;
    int ws_pending = 0;
    while (i < n)
    {
        size_t run_start = i;
        while (i < n && s[i] != '&') i++;
        size_t run_len = i - run_start;
        if (run_len > 0)
        {
            const char *run = s + run_start;
            if (utf8_valid(run, run_len))
            {
                for (size_t j = 0; j < run_len; j++)
                {
                    if (title_append_byte(b, (unsigned char)run[j],
                                          &ws_pending) != 0)
                        return -1;
                }
            }
            else
            {
                char buf[4];
                for (size_t j = 0; j < run_len; j++)
                {
                    unsigned char c = (unsigned char)run[j];
                    if (c < 0x80)
                    {
                        if (title_append_byte(b, c, &ws_pending) != 0)
                            return -1;
                    }
                    else
                    {
                        size_t blen = utf8_encode_cp(cp1252_cp(c), buf);
                        if (blen == 0)
                        {
                            buf[0] = (char)0xEF; buf[1] = (char)0xBF;
                            buf[2] = (char)0xBD; blen = 3; /* U+FFFD */
                        }
                        for (size_t k = 0; k < blen; k++)
                        {
                            if (title_append_byte(b, (unsigned char)buf[k],
                                                  &ws_pending) != 0)
                                return -1;
                        }
                    }
                }
            }
        }
        if (i >= n) break;
        char ebuf[4];
        size_t elen = 0;
        size_t consumed = 0;
        if (decode_entity(s + i, n - i, ebuf, &elen, &consumed))
        {
            for (size_t k = 0; k < elen; k++)
            {
                if (title_append_byte(b, (unsigned char)ebuf[k],
                                      &ws_pending) != 0)
                    return -1;
            }
            i += consumed;
            continue;
        }
        /* not an entity: emit the '&' literally */
        if (title_append_byte(b, (unsigned char)'&', &ws_pending) != 0)
            return -1;
        i++;
    }
    return 0;
}
