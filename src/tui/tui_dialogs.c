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

#include "tui_dialogs.h"
#include "../utils/string_utils.h"

TuiModal *tui_modal_open(TuiModalKind kind, const char *title,
                         const char *body, TuiEvent *event)
{
    TuiModal *m = calloc(1, sizeof(TuiModal));
    if (!m) return NULL;
    m->kind = kind;
    m->title = str_dup(title ? title : "");
    m->body = body ? str_dup(body) : NULL;
    m->event = event;
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
    if (tui_modal_needs_input(kind))
    {
        m->input = tui_input_create(0);
        if (!m->input)
        {
            free(m->body);
            free(m->title);
            free(m);
            return NULL;
        }
    }
    return m;
}

void tui_modal_close(TuiModal *m)
{
    if (!m) return;
    tui_input_destroy(m->input);
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
    return kind == TUI_MODAL_PASSWORD || kind == TUI_MODAL_ASK_USER;
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
            size_t n = 0;
            if (c <= 0x7f)
            {
                buf[0] = (char)c;
                n = 1;
            }
            else if (c <= 0x7ff)
            {
                buf[0] = (char)(0xc0 | (c >> 6));
                buf[1] = (char)(0x80 | (c & 0x3f));
                n = 2;
            }
            else
            {
                buf[0] = (char)(0xe0 | (c >> 12));
                buf[1] = (char)(0x80 | ((c >> 6) & 0x3f));
                buf[2] = (char)(0x80 | (c & 0x3f));
                n = 3;
            }
            buf[n] = '\0';
            (void)tui_input_insert(m->input, buf);
            return TUI_MODAL_ACTION_NONE;
        }
        return TUI_MODAL_ACTION_NONE;

    default:
        return TUI_MODAL_ACTION_NONE;
    }
}
