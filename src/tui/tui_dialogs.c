/*
 * tui_dialogs.c - modal state machine. Enter/Esc semantics per kind:
 * password and ask_user collect input and submit/cancel; approval answers
 * the event with allow/deny; confirm-quit maps y/n. The round-trip event
 * is answered on completion; the modal itself is freed by the caller.
 * Depends on: tui_input for the text buffer.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "tui_dialogs.h"
#include "../utils/string_utils.h"

/* Split a newline-joined body into the picker's item array. */
static int picker_split_items(TuiModal *m)
{
    if (!m->body || !m->body[0]) return 0;
    size_t n = strlen(m->body);
    int cap = 1;
    for (size_t i = 0; i < n; i++)
        if (m->body[i] == '\n') cap++;
    m->items = calloc((size_t)cap, sizeof(char *));
    if (!m->items) return -1;
    int count = 0;
    const char *start = m->body;
    const char *q = m->body;
    while (1)
    {
        if (*q == '\n' || *q == '\0')
        {
            size_t len = (size_t)(q - start);
            if (len > 0)
            {
                m->items[count] = strndup(start, len);
                if (!m->items[count])
                {
                    for (int i = 0; i < count; i++) free(m->items[i]);
                    free(m->items);
                    m->items = NULL;
                    return -1;
                }
                count++;
            }
            if (*q == '\0') break;
            start = q + 1;
        }
        q++;
    }
    m->item_count = count;
    return 0;
}

static int picker_init_visible(TuiModal *m)
{
    int cap = m->item_count > 0 ? m->item_count : 1;
    m->visible = calloc((size_t)cap, sizeof(int));
    if (!m->visible) return -1;
    m->visible_count = m->item_count;
    for (int i = 0; i < m->item_count; i++) m->visible[i] = i;
    m->cursor = 0;
    m->top = 0;
    m->window = 8;
    m->selection = -1;
    return 0;
}

TuiModal *tui_modal_open(TuiModalKind kind, const char *title,
                         const char *body, TuiEvent *event)
{
    TuiModal *m = calloc(1, sizeof(TuiModal));
    if (!m) return NULL;
    m->kind = kind;
    m->title = str_dup(title ? title : "");
    m->body = body ? str_dup(body) : NULL;
    m->event = event;
    m->selection = -1;
    if (!m->title)
    {
        free(m);
        return NULL;
    }
    if (body && !m->body)
    {
        free(m->title);
        free(m);
        return NULL;
    }
    if (kind == TUI_MODAL_PICKER)
    {
        if (picker_split_items(m) != 0 || picker_init_visible(m) != 0)
        {
            tui_modal_close(m);
            return NULL;
        }
    }
    if (tui_modal_needs_input(kind))
    {
        m->input = tui_input_create(0);
        if (!m->input)
        {
            tui_modal_close(m);
            return NULL;
        }
    }
    return m;
}

void tui_modal_close(TuiModal *m)
{
    if (!m) return;
    tui_input_destroy(m->input);
    for (int i = 0; i < m->item_count; i++) free(m->items[i]);
    free(m->items);
    free(m->visible);
    free(m->body);
    free(m->title);
    free(m);
}

TuiModalKind tui_modal_kind(const TuiModal *m)
{
    return m ? m->kind : TUI_MODAL_NONE;
}

const char *tui_modal_title(const TuiModal *m)
{
    return (m && m->title) ? m->title : "";
}

const char *tui_modal_body(const TuiModal *m)
{
    return m ? m->body : NULL;
}

const char *tui_modal_text(const TuiModal *m)
{
    return (m && m->input) ? tui_input_text(m->input) : "";
}

void tui_modal_mask(const char *text, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    size_t in = 0;
    size_t o = 0;
    size_t n = text ? strlen(text) : 0;
    while (in < n && o + 4 <= cap) /* room for one mask glyph + NUL */
    {
        if (text[in] == ' ')
        {
            out[o++] = ' ';
            in++;
        }
        else
        {
            /* One bullet per codepoint, whatever its byte length */
            memcpy(out + o, "\xE2\x80\xA2", 3);
            o += 3;
            in++;
            while (in < n && ((unsigned char)text[in] & 0xC0) == 0x80) in++;
        }
    }
    out[o] = '\0';
}

int tui_modal_needs_input(TuiModalKind kind)
{
    return kind == TUI_MODAL_PASSWORD || kind == TUI_MODAL_ASK_USER ||
           kind == TUI_MODAL_PICKER;
}

/* ---- picker helpers ---- */

static void utf8_encode(int c, char *buf)
{
    if (c <= 0x7f)
    {
        buf[0] = (char)c;
        buf[1] = '\0';
    }
    else if (c <= 0x7ff)
    {
        buf[0] = (char)(0xc0 | (c >> 6));
        buf[1] = (char)(0x80 | (c & 0x3f));
        buf[2] = '\0';
    }
    else
    {
        buf[0] = (char)(0xe0 | (c >> 12));
        buf[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        buf[2] = (char)(0x80 | (c & 0x3f));
        buf[3] = '\0';
    }
}

/* Case-insensitive substring match; ASCII-only folding (filters are typed
 * by the user, items are ASCII provider/model names). */
static int pick_contains(const char *haystack, const char *needle)
{
    if (!needle || !needle[0]) return 1;
    if (!haystack) return 0;
    size_t hn = strlen(haystack);
    size_t nn = strlen(needle);
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++)
    {
        int match = 1;
        for (size_t j = 0; j < nn; j++)
        {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j]))
            {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

/* Rebuild the visible[] list against the filter; on allocation failure
 * keep the previous list (best-effort filtering, same as history). */
static void picker_rebuild(TuiModal *m)
{
    const char *filter = tui_modal_text(m);
    if (!filter || !filter[0])
    {
        m->visible_count = m->item_count;
        for (int i = 0; i < m->item_count; i++) m->visible[i] = i;
        m->cursor = 0;
        m->top = 0;
        return;
    }
    int cap = m->item_count > 0 ? m->item_count : 1;
    int *nv = calloc((size_t)cap, sizeof(int));
    if (!nv) return; /* keep the old visible list */
    int n = 0;
    for (int i = 0; i < m->item_count; i++)
        if (pick_contains(m->items[i], filter)) nv[n++] = i;
    free(m->visible);
    m->visible = nv;
    m->visible_count = n;
    if (m->cursor >= m->visible_count) m->cursor = m->visible_count > 0
                                                    ? m->visible_count - 1 : 0;
    m->top = 0;
}

static void picker_move(TuiModal *m, int delta)
{
    if (m->visible_count == 0) return;
    m->cursor += delta;
    if (m->cursor < 0) m->cursor = 0;
    if (m->cursor >= m->visible_count) m->cursor = m->visible_count - 1;
    if (m->window < 1) m->window = 1;
    if (m->cursor < m->top) m->top = m->cursor;
    if (m->cursor >= m->top + m->window) m->top = m->cursor - m->window + 1;
    if (m->top + m->window > m->visible_count && m->visible_count > 0)
    {
        int t = m->visible_count - m->window;
        if (t < 0) t = 0;
        if (m->top > t) m->top = t;
    }
}

TuiModalAction tui_modal_dispatch_key(TuiModal *m, int c)
{
    if (!m) return TUI_MODAL_ACTION_NONE;

    switch (m->kind)
    {
    case TUI_MODAL_NOTICE:
        return TUI_MODAL_ACTION_ANSWERED; /* any key dismisses */

    case TUI_MODAL_CONFIRM_QUIT:
        if (c == 'y' || c == 'Y')
        {
            if (m->event)
                (void)tui_event_answer(m->event, NULL, 1);
            return TUI_MODAL_ACTION_QUIT;
        }
        return TUI_MODAL_ACTION_ANSWERED; /* n or Esc closes */

    case TUI_MODAL_CONFIRM:
        /* Generic yes/no (menu confirmations): records the choice in
         * selection (1 = yes) and closes without quitting. */
        if (c == 'y' || c == 'Y' || c == 10 || c == 13)
        {
            m->selection = 1;
            if (m->event) (void)tui_event_answer(m->event, NULL, 1);
            return TUI_MODAL_ACTION_ANSWERED;
        }
        if (c == 'n' || c == 'N' || c == 27)
        {
            m->selection = 0;
            if (m->event) (void)tui_event_answer(m->event, NULL, 0);
            return TUI_MODAL_ACTION_ANSWERED;
        }
        return TUI_MODAL_ACTION_NONE; /* cannot be dismissed otherwise */

    case TUI_MODAL_APPROVAL:
        if (c == 'y' || c == 'Y' || c == 10 || c == 13)
        {
            if (m->event) (void)tui_event_answer(m->event, NULL, 1);
            return TUI_MODAL_ACTION_ANSWERED;
        }
        if (c == 'n' || c == 'N' || c == 27)
        {
            if (m->event) (void)tui_event_answer(m->event, NULL, 0);
            return TUI_MODAL_ACTION_ANSWERED;
        }
        return TUI_MODAL_ACTION_NONE; /* cannot be dismissed otherwise */

    case TUI_MODAL_PICKER:
        if (c == 10 || c == 13) /* Enter: commit the selection */
        {
            m->selection = m->visible_count > 0 ? m->visible[m->cursor] : -1;
            if (m->event)
                (void)tui_event_answer(m->event,
                                       tui_modal_picker_selected(m), 0);
            return TUI_MODAL_ACTION_ANSWERED;
        }
        if (c == 27) /* Esc: cancel */
        {
            m->selection = -1;
            if (m->event) (void)tui_event_answer(m->event, NULL, 0);
            return TUI_MODAL_ACTION_ANSWERED;
        }
        if (c == TUI_PICKER_KEY_UP || c == 16) { picker_move(m, -1); return TUI_MODAL_ACTION_NONE; }
        if (c == TUI_PICKER_KEY_DOWN || c == 14) { picker_move(m, 1); return TUI_MODAL_ACTION_NONE; }
        if (c == TUI_PICKER_KEY_PGUP) { picker_move(m, -(m->window > 1 ? m->window - 1 : 1)); return TUI_MODAL_ACTION_NONE; }
        if (c == TUI_PICKER_KEY_PGDOWN) { picker_move(m, (m->window > 1 ? m->window - 1 : 1)); return TUI_MODAL_ACTION_NONE; }
        if (c == 127 || c == 8) /* backspace edits the filter */
        {
            (void)tui_input_backspace(m->input);
            picker_rebuild(m);
            return TUI_MODAL_ACTION_NONE;
        }
        if (c >= 32)
        {
            char buf[8];
            utf8_encode(c, buf);
            (void)tui_input_insert(m->input, buf);
            picker_rebuild(m);
            return TUI_MODAL_ACTION_NONE;
        }
        return TUI_MODAL_ACTION_NONE;

    case TUI_MODAL_PASSWORD:
    case TUI_MODAL_ASK_USER:
        if (c == 10 || c == 13) /* Enter: submit */
        {
            if (m->event)
                (void)tui_event_answer(m->event, tui_input_text(m->input), 0);
            return TUI_MODAL_ACTION_ANSWERED;
        }
        if (c == 27) /* Esc: cancel */
        {
            if (m->event) (void)tui_event_answer(m->event, NULL, 0);
            return TUI_MODAL_ACTION_ANSWERED;
        }
        if (c == 127 || c == 8) /* backspace */
        {
            (void)tui_input_backspace(m->input);
            return TUI_MODAL_ACTION_NONE;
        }
        if (c >= 32)
        {
            char buf[8];
            utf8_encode(c, buf);
            (void)tui_input_insert(m->input, buf);
            return TUI_MODAL_ACTION_NONE;
        }
        return TUI_MODAL_ACTION_NONE;

    default:
        return TUI_MODAL_ACTION_NONE;
    }
}

/* ---- picker accessors ---- */

int tui_modal_picker_item_count(const TuiModal *m)
{
    return m ? m->item_count : 0;
}

int tui_modal_picker_visible_count(const TuiModal *m)
{
    return m ? m->visible_count : 0;
}

const char *tui_modal_picker_visible_at(const TuiModal *m, int row)
{
    if (!m || row < 0 || row >= m->visible_count) return NULL;
    return m->items[m->visible[row]];
}

int tui_modal_picker_cursor(const TuiModal *m)
{
    return m ? m->cursor : 0;
}

int tui_modal_picker_top(const TuiModal *m)
{
    return m ? m->top : 0;
}

void tui_modal_picker_set_window(TuiModal *m, int rows)
{
    if (!m) return;
    if (rows < 1) rows = 1;
    m->window = rows;
    picker_move(m, 0);
}

const char *tui_modal_picker_selected(const TuiModal *m)
{
    if (!m || m->selection < 0 || m->selection >= m->item_count) return NULL;
    return m->items[m->selection];
}
