/*
 * tui_stream.c - chunk classifier. The stream layers emit explicit
 * "<think>\n", thinking text, and "\n</think>\n\n" deltas; without
 * splitting, the closing delta would land in the assistant block as three
 * stray newlines (the "3 spaces between thinking and reply" bug). Slices
 * are trimmed of the separator whitespace the provider wraps around the
 * markers, so one marker chunk can be dropped entirely.
 * Depends on: tui_stream.h.
 */

#include <string.h>

#include "tui_stream.h"

static size_t part_trim_trailing(const char *s, size_t n)
{
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == ' ')) n--;
    return n;
}

static size_t part_trim_leading(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && (s[i] == '\n' || s[i] == ' ')) i++;
    return n - i;
}

static int add_part(TuiStreamPart parts[2], int *count,
                    TuiStreamPartKind kind, const char *start, size_t len)
{
    if (len == 0) return 0;
    parts[*count].kind = kind;
    parts[*count].start = start;
    parts[*count].len = len;
    (*count)++;
    return 0;
}

int tui_stream_split(const char *chunk, int in_think,
                     TuiStreamPart parts[2], int *out_think)
{
    if (!chunk || !parts || !out_think) return 0;

    size_t len = strlen(chunk);
    const char *open = strstr(chunk, "<think>");
    const char *close = strstr(chunk, "</think>");
    int count = 0;

    if (open && (!close || open < close))
    {
        /* Opening marker: before it is content, after it is think */
        size_t pre = (size_t)(open - chunk);
        size_t pre_len = part_trim_trailing(chunk, pre);
        (void)add_part(parts, &count, TUI_STREAM_PART_CONTENT, chunk, pre_len);

        const char *post = open + 7;
        size_t post_len = len - (size_t)(post - chunk);
        size_t trimmed = part_trim_leading(post, post_len);
        (void)add_part(parts, &count, TUI_STREAM_PART_THINK,
                       post + (post_len - trimmed), trimmed);
        *out_think = 1;
        return count;
    }

    if (close)
    {
        /* Closing marker: before it is think, after it is content */
        size_t pre = (size_t)(close - chunk);
        size_t pre_len = part_trim_trailing(chunk, pre);
        (void)add_part(parts, &count, TUI_STREAM_PART_THINK, chunk, pre_len);

        const char *post = close + 8;
        size_t post_len = len - (size_t)(post - chunk);
        size_t trimmed = part_trim_leading(post, post_len);
        (void)add_part(parts, &count, TUI_STREAM_PART_CONTENT,
                       post + (post_len - trimmed), trimmed);
        *out_think = 0;
        return count;
    }

    (void)add_part(parts, &count,
                   in_think ? TUI_STREAM_PART_THINK : TUI_STREAM_PART_CONTENT,
                   chunk, len);
    *out_think = in_think;
    return count;
}
