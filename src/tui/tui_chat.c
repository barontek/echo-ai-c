/*
 * tui_chat.c - scrollback model. The commit site is block append: grow the
 * block array, fill the slot, then bump count — a failure at any point
 * leaves the scrollback exactly as before the call (fault-injection tested
 * via TUI_CHAT_TEST). Wrapping is greedy word wrap with mid-word breaks
 * for overlong tokens; hard breaks on '\n'. Depends on: string_utils.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tui_chat.h"
#include "../utils/string_utils.h"

#ifdef TUI_CHAT_TEST
/* Fault-injection shims: the test TU provides these so allocation
 * failures can be forced at each commit site. */
void *tui_chat_test_calloc(size_t nmemb, size_t size);
char *tui_chat_test_strdup(const char *s);
#define calloc tui_chat_test_calloc
#define str_dup tui_chat_test_strdup
#endif

typedef struct {
    TuiBlockKind kind;
    char *text;
    char *title; /* header title (tool name for tool blocks); owned */
    TuiCollapseState collapse; /* AUTO unless the user toggled the block */
    size_t wrap_width;  /* width the cached offsets were computed for; 0 = none */
    size_t wrap_lines;  /* wrapped line count for wrap_width */
    size_t *wrap_starts; /* cached line-start offsets (+1 sentinel); owned */
} TuiChatBlock;

struct TuiChat {
    TuiChatBlock *blocks;
    size_t count;
    size_t cap;
    int streaming_open;
    TuiBlockKind streaming_kind;
};

TuiChat *tui_chat_create(void)
{
    return calloc(1, sizeof(TuiChat));
}

void tui_chat_destroy(TuiChat *chat)
{
    if (!chat) return;
    for (size_t i = 0; i < chat->count; i++)
    {
        free(chat->blocks[i].text);
        free(chat->blocks[i].title);
        free(chat->blocks[i].wrap_starts);
    }
    free(chat->blocks);
    free(chat);
}

static void invalidate_wrap(TuiChatBlock *blk)
{
    free(blk->wrap_starts);
    blk->wrap_starts = NULL;
    blk->wrap_width = 0;
    blk->wrap_lines = 0;
}

static void seal_streaming(TuiChat *chat)
{
    chat->streaming_open = 0;
}

/* Append a block; on failure the scrollback is unchanged. The array
 * growth uses allocate-copy-free (not realloc) so the fault-injection
 * shims can force a failure at the growth step itself. The title is
 * duplicated after the text; a failure there rolls back the text copy. */
static int block_append2(TuiChat *chat, TuiBlockKind kind, const char *text,
                         const char *title)
{
    if (chat->count == chat->cap)
    {
        size_t new_cap = chat->cap == 0 ? 8 : chat->cap * 2;
        if (new_cap < chat->cap) return -1; /* overflow guard */
        TuiChatBlock *nb = calloc(new_cap, sizeof(TuiChatBlock));
        if (!nb) return -1;
        /* First append: blocks is NULL and count is 0 — memcpy must not
         * see the NULL source (UBSan flags NULL+0 as UB). */
        if (chat->count > 0)
            memcpy(nb, chat->blocks, chat->count * sizeof(TuiChatBlock));
        free(chat->blocks);
        chat->blocks = nb;
        chat->cap = new_cap;
    }
    char *copy = str_dup(text ? text : "");
    if (!copy) return -1;
    char *title_copy = NULL;
    if (title)
    {
        title_copy = str_dup(title);
        if (!title_copy)
        {
            free(copy);
            return -1;
        }
    }
    chat->blocks[chat->count].kind = kind;
    chat->blocks[chat->count].text = copy;
    chat->blocks[chat->count].title = title_copy;
    chat->count++;
    return 0;
}

static int block_append(TuiChat *chat, TuiBlockKind kind, const char *text)
{
    return block_append2(chat, kind, text, NULL);
}

int tui_chat_begin_user(TuiChat *chat, const char *text)
{
    if (!chat) return -1;
    int was_streaming = chat->streaming_open;
    seal_streaming(chat);
    if (block_append(chat, TUI_BLOCK_USER, text) != 0)
    {
        /* restore the exact prior state */
        chat->streaming_open = was_streaming;
        return -1;
    }
    return 0;
}

int tui_chat_begin_stream(TuiChat *chat, TuiBlockKind kind)
{
    if (!chat) return -1;
    if (kind != TUI_BLOCK_ASSISTANT && kind != TUI_BLOCK_THINK) return -1;
    int was_streaming = chat->streaming_open;
    seal_streaming(chat);
    if (block_append(chat, kind, "") != 0)
    {
        chat->streaming_open = was_streaming;
        return -1;
    }
    chat->streaming_open = 1;
    chat->streaming_kind = kind;
    return 0;
}

int tui_chat_stream_append(TuiChat *chat, const char *delta)
{
    if (!chat || !delta) return -1;
    if (!chat->streaming_open) return 0;
    if (delta[0] == '\0') return 0;

    size_t idx = chat->count - 1;
    size_t cur = strlen(chat->blocks[idx].text);
    size_t add = strlen(delta);
    if (cur + add > SIZE_MAX - 1) return -1;
    /* allocate-copy-free: the growth must be injectable and atomic */
    char *nb = calloc(cur + add + 1, 1);
    if (!nb) return -1;
    memcpy(nb, chat->blocks[idx].text, cur);
    memcpy(nb + cur, delta, add);
    free(chat->blocks[idx].text);
    chat->blocks[idx].text = nb;
    invalidate_wrap(&chat->blocks[idx]);
    return 0;
}

void tui_chat_end_stream(TuiChat *chat)
{
    if (chat) seal_streaming(chat);
}

int tui_chat_append_tool(TuiChat *chat, const char *name, const char *result)
{
    if (!chat) return -1;
    char *line = NULL;
    if (asprintf(&line, "%s: %s", name ? name : "(tool)", result ? result : "") < 0)
        return -1;
    int rc = block_append(chat, TUI_BLOCK_TOOL, line);
    free(line);
    return rc;
}

int tui_chat_begin_tool(TuiChat *chat, const char *name)
{
    if (!chat) return -1;
    return block_append2(chat, TUI_BLOCK_TOOL, "", name);
}

int tui_chat_tool_finish(TuiChat *chat, const char *name, const char *result)
{
    if (!chat) return -1;
    if (chat->count > 0)
    {
        TuiChatBlock *last = &chat->blocks[chat->count - 1];
        if (last->kind == TUI_BLOCK_TOOL && last->text[0] == '\0')
        {
            char *copy = str_dup(result ? result : "");
            if (!copy) return -1; /* block stays pending */
            free(last->text);
            last->text = copy;
            invalidate_wrap(last);
            return 0;
        }
    }
    return tui_chat_append_tool(chat, name, result);
}

int tui_chat_append_error(TuiChat *chat, const char *text)
{
    if (!chat) return -1;
    return block_append(chat, TUI_BLOCK_ERROR, text);
}

size_t tui_chat_block_count(const TuiChat *chat)
{
    return chat ? chat->count : 0;
}

TuiBlockKind tui_chat_block_kind(const TuiChat *chat, size_t idx)
{
    if (!chat || idx >= chat->count) return TUI_BLOCK_ERROR;
    return chat->blocks[idx].kind;
}

const char *tui_chat_block_text(const TuiChat *chat, size_t idx)
{
    if (!chat || idx >= chat->count) return "";
    return chat->blocks[idx].text;
}

const char *tui_chat_block_title(const TuiChat *chat, size_t idx)
{
    if (!chat || idx >= chat->count) return NULL;
    return chat->blocks[idx].title;
}

/* ---- wrapping ---- */

/* Display width of one codepoint, locale-independent. wcwidth(3) is
 * unusable here: it returns -1 for everything non-ASCII in the C locale,
 * which is what tests and minimal installs run in. These ranges
 * approximate standard terminal behavior deterministically. */
static int cp_display_width(uint32_t cp)
{
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return 0; /* controls */
    if ((cp >= 0x0300 && cp <= 0x036f) ||   /* combining diacritics */
        (cp >= 0x1ab0 && cp <= 0x1aff) ||
        (cp >= 0x1dc0 && cp <= 0x1dff) ||
        (cp >= 0x20d0 && cp <= 0x20ff) ||
        (cp >= 0xfe00 && cp <= 0xfe0f) ||   /* variation selectors */
        (cp >= 0xfe20 && cp <= 0xfe2f))
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115f) ||   /* hangul jamo */
        (cp >= 0x2e80 && cp <= 0x303e) ||
        (cp >= 0x3041 && cp <= 0x33ff) ||
        (cp >= 0x3400 && cp <= 0x4dbf) ||
        (cp >= 0x4e00 && cp <= 0x9fff) ||   /* CJK unified */
        (cp >= 0xa000 && cp <= 0xa4cf) ||
        (cp >= 0xac00 && cp <= 0xd7a3) ||   /* hangul syllables */
        (cp >= 0xf900 && cp <= 0xfaff) ||
        (cp >= 0xfe30 && cp <= 0xfe4f) ||
        (cp >= 0xff00 && cp <= 0xff60) ||   /* fullwidth forms */
        (cp >= 0xffe0 && cp <= 0xffe6) ||
        (cp >= 0x1f300 && cp <= 0x1f64f) || /* emoji pictographs */
        (cp >= 0x1f680 && cp <= 0x1f6ff) ||
        (cp >= 0x1f900 && cp <= 0x1f9ff) ||
        (cp >= 0x20000 && cp <= 0x2fffd) ||
        (cp >= 0x30000 && cp <= 0x3fffd))
        return 2;
    return 1;
}

/* Decode one UTF-8 codepoint; a truncated sequence decodes as its lead
 * byte so malformed input can never stall the walker. */
static size_t utf8_decode_cp(const char *s, size_t len, uint32_t *cp)
{
    unsigned char c = (unsigned char)s[0];
    size_t n;
    if (c < 0x80)
    {
        *cp = c;
        return 1;
    }
    if ((c & 0xe0) == 0xc0) { *cp = c & 0x1f; n = 2; }
    else if ((c & 0xf0) == 0xe0) { *cp = c & 0x0f; n = 3; }
    else { *cp = c & 0x07; n = 4; }
    if (len < n)
    {
        *cp = c;
        return 1;
    }
    for (size_t k = 1; k < n; k++)
        *cp = (*cp << 6) | ((unsigned char)s[k] & 0x3f);
    return n;
}

/* Greedy word wrap by display columns; see the header for the contract. */
size_t tui_chat_wrap(const char *text, size_t width,
                     size_t *line_starts, size_t cap)
{
    size_t lines = 1;
    if (cap > 0) line_starts[0] = 0;
    if (width < 1) width = 1;

    size_t len = strlen(text);
    size_t i = 0;
    size_t col = 0;        /* columns of completed words+spaces on the line */
    size_t word_start = 0; /* offset where the current word began */
    size_t word_len = 0;   /* columns of the current word */

    while (i < len)
    {
        if (text[i] == '\n')
        {
            if (lines < cap) line_starts[lines] = i + 1;
            lines++;
            i++;
            col = 0;
            word_start = i;
            word_len = 0;
            continue;
        }
        if (text[i] == ' ')
        {
            /* A space that would overflow the line starts a new line
             * instead (keeps lines <= width; the trailing space is
             * invisible when rendered). */
            if (col + word_len + 1 > width && col > 0)
            {
                if (lines < cap) line_starts[lines] = i + 1;
                lines++;
                col = 0;
                word_start = i + 1;
                word_len = 0;
            }
            else
            {
                col += word_len + 1; /* word + the space */
                word_start = i + 1;
                word_len = 0;
            }
            i++;
            continue;
        }
        uint32_t cp = 0;
        size_t n = utf8_decode_cp(text + i, len - i, &cp);
        int w = cp_display_width(cp);
        size_t cw = w > 0 ? (size_t)w : 0;
        word_len += cw;
        if (col + word_len > width)
        {
            if (col > 0 && word_len <= width)
            {
                /* The whole word moves to a fresh line. */
                if (lines < cap) line_starts[lines] = word_start;
                lines++;
                col = 0;
            }
            else if (word_len > width &&
                     !(col == 0 && cw > width))
            {
                /* Mid-word break: this codepoint starts a new line. A
                 * lone codepoint wider than the line is kept in place —
                 * breaking there would loop forever. */
                if (lines < cap) line_starts[lines] = i;
                lines++;
                col = 0;
                word_start = i;
                word_len = cw;
                i += n;
                continue;
            }
            else if (col == 0)
            {
                /* Empty line and the codepoint alone exceeds the width:
                 * place it anyway but mark the line occupied, so the
                 * next codepoint wraps onto a fresh line instead of
                 * piling onto the overflowing one. */
                col = word_len;
            }
        }
        i += n;
    }
    return lines;
}

/* ---- cached block accessors ---- */

const size_t *tui_chat_block_line_starts(TuiChat *chat, size_t idx,
                                         size_t width, size_t *lines)
{
    if (!chat || idx >= chat->count)
    {
        *lines = 0;
        return NULL;
    }
    TuiChatBlock *blk = &chat->blocks[idx];
    if (!blk->wrap_starts || blk->wrap_width != width)
    {
        /* Best-effort cache: on allocation failure the line count is
         * still reported and rendering skips the content rows. */
        size_t n = tui_chat_wrap(blk->text, width, NULL, 0);
        size_t *starts = malloc((n + 1) * sizeof(size_t));
        if (starts)
        {
            (void)tui_chat_wrap(blk->text, width, starts, n + 1);
            starts[n] = strlen(blk->text);
            invalidate_wrap(blk);
            blk->wrap_starts = starts;
            blk->wrap_width = width;
            blk->wrap_lines = n;
        }
    }
    *lines = blk->wrap_starts ? blk->wrap_lines
                              : tui_chat_wrap(blk->text, width, NULL, 0);
    return blk->wrap_starts;
}

int tui_chat_block_effective_collapsed(TuiChat *chat, size_t idx,
                                       size_t width)
{
    if (!chat || idx >= chat->count) return 0;
    const TuiChatBlock *blk = &chat->blocks[idx];
    if (blk->kind != TUI_BLOCK_TOOL) return 0;
    size_t lines = 0;
    (void)tui_chat_block_line_starts(chat, idx, width, &lines);
    if (lines <= TUI_CHAT_COLLAPSE_THRESHOLD) return 0;
    return blk->collapse != TUI_COLLAPSE_OFF;
}

size_t tui_chat_block_render_lines(TuiChat *chat, size_t idx, size_t width)
{
    if (!chat || idx >= chat->count) return 0;
    const TuiChatBlock *blk = &chat->blocks[idx];
    size_t lines = 0;
    (void)tui_chat_block_line_starts(chat, idx, width, &lines);
    /* header line: every block starts with its role label */
    size_t render = 1 + lines;
    if (blk->kind == TUI_BLOCK_TOOL && lines > TUI_CHAT_COLLAPSE_THRESHOLD)
    {
        if (tui_chat_block_effective_collapsed(chat, idx, width))
            render = 1 + TUI_CHAT_COLLAPSE_THRESHOLD + 1; /* + marker */
        else
            render = 1 + lines + 1; /* + marker */
    }
    return render;
}

int tui_chat_toggle_collapse(TuiChat *chat, size_t idx, size_t width)
{
    if (!chat || idx >= chat->count) return TUI_COLLAPSE_AUTO;
    TuiChatBlock *blk = &chat->blocks[idx];
    if (blk->kind != TUI_BLOCK_TOOL) return TUI_COLLAPSE_AUTO;
    size_t lines = 0;
    (void)tui_chat_block_line_starts(chat, idx, width, &lines);
    if (lines <= TUI_CHAT_COLLAPSE_THRESHOLD) return blk->collapse; /* no-op */
    if (tui_chat_block_effective_collapsed(chat, idx, width))
        blk->collapse = TUI_COLLAPSE_OFF;
    else
        blk->collapse = TUI_COLLAPSE_ON;
    return blk->collapse;
}

size_t tui_chat_total_lines(TuiChat *chat, size_t width)
{
    if (!chat || width < 1) return 1;
    size_t total = 0;
    for (size_t b = 0; b < chat->count; b++)
    {
        total += tui_chat_block_render_lines(chat, b, width);
        total += 1; /* separator after every block — the renderer draws
                     * it as a blank row, so the scroll math counts it */
    }
    return total == 0 ? 1 : total;
}

size_t tui_chat_view_clamp(TuiChat *chat, size_t width,
                           size_t visible, size_t top)
{
    size_t total = tui_chat_total_lines(chat, width);
    if (visible < 1) visible = 1;
    size_t max_top = total > visible ? total - visible : 0;
    return top > max_top ? max_top : top;
}
