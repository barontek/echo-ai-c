/*
 * tui_stream.c - stateful think/content chunk classifier (see tui_stream.h
 * for the contract). The scanner walks a scratch buffer holding the
 * carried tail plus the new chunk, splitting at each "<think>"/"</think>"
 * marker with separator-whitespace trimming around it; a chunk carrying
 * both markers yields up to three parts. Depends on: string.h.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tui_stream.h"

#define OPEN_MARKER  "<think>"
#define CLOSE_MARKER "</think>"
#define OPEN_LEN  7
#define CLOSE_LEN 8
#define CARRY_CAP 8 /* one byte less than the longest full marker */

struct TuiStreamClassifier {
    char *scratch;    /* carry + current chunk workspace; owned */
    size_t scratch_cap;
    char carry[CARRY_CAP]; /* unresolved marker-prefix tail; owned */
    size_t carry_len;
    int in_think;
};

TuiStreamClassifier *tui_stream_classifier_create(void)
{
    return calloc(1, sizeof(TuiStreamClassifier));
}

void tui_stream_classifier_destroy(TuiStreamClassifier *cls)
{
    if (!cls) return;
    free(cls->scratch);
    free(cls);
}

/* Portable needle search: memmem() is a glibc extension (absent on macOS). */
static const char *find_marker(const char *hay, size_t haylen,
                               const char *needle, size_t needle_len)
{
    if (needle_len > haylen) return NULL;
    for (size_t i = 0; i + needle_len <= haylen; i++)
    {
        if (memcmp(hay + i, needle, needle_len) == 0)
            return hay + i;
    }
    return NULL;
}

/* Longest suffix of s[0..len) that is a strict prefix of either marker:
 * such a tail may complete into a marker once the next chunk arrives. */
static size_t carry_prefix_len(const char *s, size_t len)
{
    size_t best = 0;
    for (size_t m = 0; m < 2; m++)
    {
        const char *marker = m == 0 ? OPEN_MARKER : CLOSE_MARKER;
        size_t mlen = m == 0 ? OPEN_LEN : CLOSE_LEN;
        size_t k = len < mlen - 1 ? len : mlen - 1; /* strict prefix */
        while (k > best)
        {
            if (memcmp(s + len - k, marker, k) == 0)
                break;
            k--;
        }
        if (k > best) best = k;
    }
    return best;
}

static size_t trim_trailing(const char *s, size_t n)
{
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == ' ')) n--;
    return n;
}

static size_t skip_leading(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && (s[i] == '\n' || s[i] == ' ')) i++;
    return i;
}

static int add_part(TuiStreamPart *parts, int *count, int cap,
                    TuiStreamPartKind kind, const char *start, size_t len)
{
    if (len == 0) return 0;
    if (*count >= cap) return 0;
    parts[*count].kind = kind;
    parts[*count].start = start;
    parts[*count].len = len;
    (*count)++;
    return 0;
}

int tui_stream_classifier_feed(TuiStreamClassifier *cls, const char *chunk,
                               TuiStreamPart *parts, int cap)
{
    if (!cls || !chunk || !parts || cap < 1) return 0;
    size_t clen = strlen(chunk);
    if (clen == 0) return 0; /* nothing new: keep the carried tail */

    size_t need = cls->carry_len + clen + 1;
    if (need > cls->scratch_cap)
    {
        size_t newcap = cls->scratch_cap == 0 ? 256 : cls->scratch_cap;
        while (newcap < need)
        {
            if (newcap > SIZE_MAX / 2)
            {
                newcap = need;
                break;
            }
            newcap *= 2;
        }
        char *nb = realloc(cls->scratch, newcap);
        if (!nb)
        {
            /* Workspace growth failed: the tail cannot be merged. Ship
             * the carry and the raw chunk unclassified so no text is
             * lost, and drop the carry state. */
            int count = 0;
            if (cls->carry_len > 0)
            {
                (void)add_part(parts, &count, cap,
                               cls->in_think ? TUI_STREAM_PART_THINK
                                             : TUI_STREAM_PART_CONTENT,
                               cls->carry, cls->carry_len);
            }
            (void)add_part(parts, &count, cap, TUI_STREAM_PART_CONTENT,
                           chunk, clen);
            cls->carry_len = 0;
            return count;
        }
        cls->scratch = nb;
        cls->scratch_cap = newcap;
    }
    memcpy(cls->scratch, cls->carry, cls->carry_len);
    memcpy(cls->scratch + cls->carry_len, chunk, clen + 1);
    size_t total = cls->carry_len + clen;
    const char *buf = cls->scratch;

    int count = 0;
    size_t pos = 0;
    int state = cls->in_think;
    while (pos < total)
    {
        const char *open = find_marker(buf + pos, total - pos,
                                       OPEN_MARKER, OPEN_LEN);
        const char *close = find_marker(buf + pos, total - pos,
                                        CLOSE_MARKER, CLOSE_LEN);
        const char *marker = NULL;
        size_t marker_len = 0;
        int opens = 0;
        if (open && (!close || open <= close))
        {
            marker = open;
            marker_len = OPEN_LEN;
            opens = 1;
        }
        else if (close)
        {
            marker = close;
            marker_len = CLOSE_LEN;
        }
        if (!marker)
        {
            /* No marker in the remainder. Keep any marker-prefix tail
             * out of the emitted text so it can resolve next feed. */
            size_t rest = total - pos;
            size_t tail = carry_prefix_len(buf + pos, rest);
            (void)add_part(parts, &count, cap,
                           state ? TUI_STREAM_PART_THINK
                                 : TUI_STREAM_PART_CONTENT,
                           buf + pos, rest - tail);
            pos = total - tail;
            break;
        }
        size_t pre = (size_t)(marker - (buf + pos));
        size_t pre_len = trim_trailing(buf + pos, pre);
        (void)add_part(parts, &count, cap,
                       state ? TUI_STREAM_PART_THINK
                             : TUI_STREAM_PART_CONTENT,
                       buf + pos, pre_len);
        size_t after = pos + pre + marker_len;
        state = opens ? 1 : 0;
        after += skip_leading(buf + after, total - after);
        pos = after;
    }
    cls->in_think = state;
    cls->carry_len = total - pos;
    if (cls->carry_len > 0)
        memcpy(cls->carry, buf + pos, cls->carry_len);
    return count;
}

int tui_stream_classifier_flush(TuiStreamClassifier *cls, TuiStreamPart *part)
{
    if (!cls || !part) return 0;
    if (cls->carry_len == 0) return 0;
    part->kind = cls->in_think ? TUI_STREAM_PART_THINK
                               : TUI_STREAM_PART_CONTENT;
    part->start = cls->carry;
    part->len = cls->carry_len;
    cls->carry_len = 0;
    return 1;
}
