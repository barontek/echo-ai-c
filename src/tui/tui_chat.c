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
    }
    free(chat->blocks);
    free(chat);
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

int tui_chat_block_effective_collapsed(const TuiChat *chat, size_t idx,
                                       size_t width)
{
    if (!chat || idx >= chat->count) return 0;
    const TuiChatBlock *blk = &chat->blocks[idx];
    if (blk->kind != TUI_BLOCK_TOOL) return 0;
    size_t lines = tui_chat_wrap(blk->text, width, NULL, 0);
    if (lines <= TUI_CHAT_COLLAPSE_THRESHOLD) return 0;
    return blk->collapse != TUI_COLLAPSE_OFF;
}

size_t tui_chat_block_render_lines(const TuiChat *chat, size_t idx,
                                   size_t width)
{
    if (!chat || idx >= chat->count) return 0;
    const TuiChatBlock *blk = &chat->blocks[idx];
    size_t lines = tui_chat_wrap(blk->text, width, NULL, 0);
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
    size_t lines = tui_chat_wrap(blk->text, width, NULL, 0);
    if (lines <= TUI_CHAT_COLLAPSE_THRESHOLD) return blk->collapse; /* no-op */
    if (tui_chat_block_effective_collapsed(chat, idx, width))
        blk->collapse = TUI_COLLAPSE_OFF;
    else
        blk->collapse = TUI_COLLAPSE_ON;
    return blk->collapse;
}

/* Greedy word wrap; see the header for the contract. */
size_t tui_chat_wrap(const char *text, size_t width,
                     size_t *line_starts, size_t cap)
{
    size_t lines = 1;
    if (cap > 0) line_starts[0] = 0;

    size_t i = 0;
    size_t col = 0;        /* current line column */
    size_t word_start = 0; /* offset where the current word began */
    size_t word_len = 0;   /* columns of the current word */

    while (text[i] != '\0')
    {
        char c = text[i];
        if (c == '\n')
        {
            if (lines < cap) line_starts[lines] = i + 1;
            lines++;
            i++;
            col = 0;
            word_start = i;
            word_len = 0;
            continue;
        }
        if (c == ' ')
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
        /* Non-space byte: extend the current word */
        word_len++;
        if (col + word_len > width)
        {
            /* The word doesn't fit: wrap before its start, or mid-word
             * when the word itself exceeds the width. The word spans the
             * break, so word_len/word_start stay as they are. */
            if (col > 0 && word_len <= width)
            {
                if (lines < cap) line_starts[lines] = word_start;
                lines++;
                col = 0;
            }
            else if (word_len > width)
            {
                if (lines < cap) line_starts[lines] = i;
                lines++;
                col = 0;
                word_start = i;
                word_len = 0;
                continue; /* reprocess byte i on the new line */
            }
        }
        i++;
    }
    return lines;
}

size_t tui_chat_total_lines(const TuiChat *chat, size_t width)
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

size_t tui_chat_view_clamp(const TuiChat *chat, size_t width,
                           size_t visible, size_t top)
{
    size_t total = tui_chat_total_lines(chat, width);
    if (visible < 1) visible = 1;
    size_t max_top = total > visible ? total - visible : 0;
    return top > max_top ? max_top : top;
}
