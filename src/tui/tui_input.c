/*
 * tui_input.c - line editor model. Byte-based with codepoint-atomic
 * deletions: backspace/delete scan continuation bytes so a UTF-8
 * codepoint is never left half-deleted. Growth is doubling realloc with
 * failure preservation (old buffer kept, -1 returned). History is a
 * circular list of submitted lines, capped, with dedupe against the
 * previous entry. Depends on: string_utils (str_dup).
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tui_input.h"
#include "../utils/string_utils.h"

#ifdef TUI_INPUT_TEST
/* Fault-injection shims for the buffer-growth and history paths. */
void *tui_input_test_calloc(size_t nmemb, size_t size);
char *tui_input_test_strdup(const char *s);
#define calloc tui_input_test_calloc
#define str_dup tui_input_test_strdup
#endif

struct TuiInput {
    char *buf;
    size_t cap;
    size_t len;
    size_t cursor;
    size_t history_cap;
    size_t history_count;
    size_t history_head; /* index of oldest entry */
    char **history;
    size_t history_walk; /* entry being shown while walking; count==0 disabled */
    int walking;
    char *draft; /* buffer content saved when the walk started */
};

/* Grow the buffer; allocate-copy-free so the fault-injection shims can
 * force a failure at the growth step (realloc would bypass them). */
static int grow_buffer(TuiInput *in, size_t needed)
{
    if (needed <= in->cap) return 0;
    size_t new_cap = in->cap == 0 ? 64 : in->cap;
    while (new_cap < needed)
    {
        if (new_cap > SIZE_MAX / 2)
        {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    char *nb = calloc(new_cap, 1);
    if (!nb) return -1;
    if (in->buf)
    {
        memcpy(nb, in->buf, in->len + 1);
        free(in->buf);
    }
    in->buf = nb;
    in->cap = new_cap;
    return 0;
}

/* Move the cursor left across one whole UTF-8 codepoint. */
static void cursor_left_cp(TuiInput *in)
{
    if (in->cursor == 0) return;
    size_t i = in->cursor - 1;
    while (i > 0 && ((unsigned char)in->buf[i] & 0xC0) == 0x80) i--;
    in->cursor = i;
}

/* Move the cursor right across one whole UTF-8 codepoint. */
static void cursor_right_cp(TuiInput *in)
{
    if (in->cursor >= in->len) return;
    size_t i = in->cursor + 1;
    while (i < in->len && ((unsigned char)in->buf[i] & 0xC0) == 0x80) i++;
    in->cursor = i;
}

TuiInput *tui_input_create(size_t history_cap)
{
    TuiInput *in = calloc(1, sizeof(TuiInput));
    if (!in) return NULL;
    in->history_cap = history_cap;
    if (history_cap > 0)
    {
        in->history = calloc(history_cap, sizeof(char *));
        if (!in->history)
        {
            free(in);
            return NULL;
        }
    }
    if (grow_buffer(in, 1) != 0)
    {
        free(in->history);
        free(in);
        return NULL;
    }
    in->buf[0] = '\0';
    return in;
}

void tui_input_destroy(TuiInput *in)
{
    if (!in) return;
    for (size_t i = 0; i < in->history_count; i++)
        free(in->history[i]);
    free(in->history);
    free(in->draft);
    free(in->buf);
    free(in);
}

int tui_input_insert(TuiInput *in, const char *text)
{
    if (!in || !text) return -1;
    size_t n = strlen(text);
    if (n == 0) return 0;

    if (grow_buffer(in, in->len + n + 1) != 0) return -1;

    memmove(in->buf + in->cursor + n, in->buf + in->cursor, in->len - in->cursor + 1);
    memcpy(in->buf + in->cursor, text, n);
    in->len += n;
    in->cursor += n;
    return 0;
}

int tui_input_backspace(TuiInput *in)
{
    if (!in || in->cursor == 0) return 0;
    size_t start = in->cursor;
    cursor_left_cp(in);
    size_t n = start - in->cursor;
    memmove(in->buf + in->cursor, in->buf + start, in->len - start + 1);
    in->len -= n;
    return (int)n;
}

int tui_input_delete(TuiInput *in)
{
    if (!in || in->cursor >= in->len) return 0;
    size_t start = in->cursor;
    cursor_right_cp(in);
    size_t n = in->cursor - start;
    memmove(in->buf + start, in->buf + in->cursor, in->len - in->cursor + 1);
    in->len -= n;
    in->cursor = start;
    return (int)n;
}

void tui_input_move(TuiInput *in, long steps)
{
    if (!in) return;
    if (steps > 0)
    {
        for (long i = 0; i < steps; i++) cursor_right_cp(in);
    }
    else
    {
        for (long i = 0; i < -steps; i++) cursor_left_cp(in);
    }
}

void tui_input_home(TuiInput *in)
{
    if (in) in->cursor = 0;
}

void tui_input_end(TuiInput *in)
{
    if (in) in->cursor = in->len;
}

void tui_input_clear(TuiInput *in)
{
    if (!in) return;
    in->buf[0] = '\0';
    in->len = 0;
    in->cursor = 0;
}

static int is_space_or_boundary(char c)
{
    return c == ' ' || c == '\t';
}

int tui_input_delete_word(TuiInput *in)
{
    if (!in) return 0;
    size_t pos = in->cursor;
    /* Skip trailing whitespace, then the word itself */
    while (pos > 0 && is_space_or_boundary(in->buf[pos - 1])) pos--;
    while (pos > 0 && !is_space_or_boundary(in->buf[pos - 1])) pos--;
    size_t n = in->cursor - pos;
    if (n == 0) return 0;
    memmove(in->buf + pos, in->buf + in->cursor, in->len - in->cursor + 1);
    in->len -= n;
    in->cursor = pos;
    return (int)n;
}

int tui_input_set_text(TuiInput *in, const char *text)
{
    if (!in) return -1;
    size_t n = text ? strlen(text) : 0;
    if (grow_buffer(in, n + 1) != 0) return -1;
    if (n > 0) memcpy(in->buf, text, n);
    in->buf[n] = '\0';
    in->len = n;
    in->cursor = n;
    return 0;
}

const char *tui_input_text(const TuiInput *in)
{
    return in ? in->buf : "";
}

size_t tui_input_len(const TuiInput *in)
{
    return in ? in->len : 0;
}

size_t tui_input_cursor(const TuiInput *in)
{
    return in ? in->cursor : 0;
}

static void history_push(TuiInput *in, const char *line)
{
    if (in->history_cap == 0) return;
    /* Dedupe against the most recent entry */
    if (in->history_count > 0)
    {
        size_t last = (in->history_head + in->history_count - 1) % in->history_cap;
        if (strcmp(in->history[last], line) == 0) return;
    }
    size_t slot;
    if (in->history_count < in->history_cap)
    {
        slot = (in->history_head + in->history_count) % in->history_cap;
        in->history_count++;
    }
    else
    {
        slot = in->history_head; /* overwrite the oldest */
        in->history_head = (in->history_head + 1) % in->history_cap;
    }
    free(in->history[slot]);
    in->history[slot] = str_dup(line);
    /* A failed str_dup silently skips the entry: history is best-effort */
}

char *tui_input_submit(TuiInput *in)
{
    if (!in) return NULL;
    if (in->len == 0) return NULL;

    char *out = str_dup(in->buf);
    if (!out) return NULL;
    history_push(in, in->buf);
    in->buf[0] = '\0';
    in->len = 0;
    in->cursor = 0;
    in->walking = 0;
    free(in->draft);
    in->draft = NULL;
    return out;
}

const char *tui_input_history_back(TuiInput *in)
{
    if (!in || in->history_count == 0) return NULL;
    if (!in->walking)
    {
        /* Save the live edit so forward-walking can restore it */
        in->draft = str_dup(in->buf);
        in->walking = 1;
        in->history_walk = 0; /* 0 = newest */
    }
    else if (in->history_walk >= in->history_count - 1)
    {
        return NULL; /* at the oldest entry */
    }
    else
    {
        in->history_walk++;
    }
    size_t idx = (in->history_head + in->history_count - 1 - in->history_walk) % in->history_cap;
    if (tui_input_set_text(in, in->history[idx]) != 0)
        return NULL;
    return in->buf;
}

const char *tui_input_history_forward(TuiInput *in)
{
    if (!in || !in->walking) return NULL;
    if (in->history_walk == 0)
    {
        /* Back to the live edit: restore the saved draft */
        if (tui_input_set_text(in, in->draft) != 0)
            return NULL;
        in->walking = 0;
        return in->buf;
    }
    in->history_walk--;
    size_t idx = (in->history_head + in->history_count - 1 - in->history_walk) % in->history_cap;
    if (tui_input_set_text(in, in->history[idx]) != 0)
        return NULL;
    return in->buf;
}

void tui_input_reset_history_walk(TuiInput *in)
{
    if (!in) return;
    in->walking = 0;
    free(in->draft);
    in->draft = NULL;
}
