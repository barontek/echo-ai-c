/*
 * tui_dialogs.h - modal dialog controller: kind, title/body text, the
 * round-trip event to answer, and the password input model. The state
 * machine and masking logic are pure (unit-testable); plane rendering
 * lives in tui.c. Depends on: tui_events.h, tui_input.h.
 */

#ifndef ECHO_TUI_DIALOGS_H
#define ECHO_TUI_DIALOGS_H

#include "tui_events.h"
#include "tui_input.h"

typedef enum {
    TUI_MODAL_NONE,
    TUI_MODAL_PASSWORD,   /* masked input; Enter submits, Esc cancels */
    TUI_MODAL_CONFIRM_QUIT, /* y = quit, n/Esc = cancel */
    TUI_MODAL_ASK_USER,   /* Enter = submit answer, Esc = cancel question */
    TUI_MODAL_APPROVAL,   /* y = allow, n = deny (no dismiss) */
    TUI_MODAL_NOTICE      /* any key dismisses (unlock errors etc.) */
} TuiModalKind;

/* What the UI loop must do after a key was dispatched to the modal. */
typedef enum {
    TUI_MODAL_ACTION_NONE,    /* modal still open; keep rendering it */
    TUI_MODAL_ACTION_ANSWERED, /* answer written; close the modal (NONE) */
    TUI_MODAL_ACTION_QUIT      /* confirm-quit accepted; exit the app */
} TuiModalAction;

typedef struct {
    TuiModalKind kind;
    char *title;   /* owned */
    char *body;    /* owned; question text / tool args */
    TuiEvent *event; /* round-trip event to answer; owned by caller */
    int event_result; /* approval result to write when answering */
    TuiInput *input;  /* password / answer text buffer (owned) */
} TuiModal;

/**
 * tui_modal_open - open a modal
 * @kind: modal kind.
 * @title: modal title, borrowed (copied).
 * @body: body text, borrowed (copied); NULL allowed.
 * @event: round-trip event to answer on submit/cancel, or NULL.
 *   Not owned by the modal.
 *
 * Return: caller-owned TuiModal (release with tui_modal_close()), or NULL
 *   on allocation failure.
 */
TuiModal *tui_modal_open(TuiModalKind kind, const char *title,
                         const char *body, TuiEvent *event);

/**
 * tui_modal_close - release a modal
 * @m: modal to release, or NULL (no-op).
 *
 * Frees owned strings and the input buffer. The attached event is NOT
 * freed (its owner does that) and NOT answered by this call.
 *
 * Return: void.
 */
void tui_modal_close(TuiModal *m);

/**
 * tui_modal_kind - the modal's kind
 * @m: modal; non-NULL.
 *
 * Return: kind.
 */
TuiModalKind tui_modal_kind(const TuiModal *m);

/**
 * tui_modal_title / tui_modal_body - modal text accessors
 * @m: modal; non-NULL.
 *
 * Return: borrowed NUL-terminated strings ("" / NULL for empty).
 */
const char *tui_modal_title(const TuiModal *m);
const char *tui_modal_body(const TuiModal *m);

/**
 * tui_modal_dispatch_key - feed a key to the modal state machine
 * @m: modal; non-NULL.
 * @c: the key: 0-31 control bytes, ASCII printable, or a Unicode
 *   codepoint already decoded by the input layer.
 *
 * Text keys append to the password/answer input. Enter submits, Esc
 * cancels, backspace edits. On submit/cancel the round-trip event (when
 * attached) is answered via tui_event_answer() and the action says the
 * modal is done. The modal remains allocated; the caller closes it.
 *
 * Return: TUI_MODAL_ACTION_NONE while open, ANSWERED when finished,
 *   QUIT only for a confirmed CONFIRM_QUIT.
 */
TuiModalAction tui_modal_dispatch_key(TuiModal *m, int c);

/**
 * tui_modal_text - the modal's input text
 * @m: modal; non-NULL.
 *
 * Return: borrowed input buffer ("" when the modal has no input).
 */
const char *tui_modal_text(const TuiModal *m);

/**
 * tui_modal_mask - replace every character with a mask glyph
 * @text: input, borrowed.
 * @out: caller-owned buffer receiving the masked string.
 * @cap: capacity of @out.
 *
 * '•' for non-space characters, ' ' preserved (a password of spaces
 * stays visibly aligned). The result is always NUL-terminated; overlong
 * input is truncated.
 *
 * Return: void.
 */
void tui_modal_mask(const char *text, char *out, size_t cap);

/**
 * tui_modal_needs_input - does this kind collect typed input?
 * @kind: modal kind.
 *
 * Return: 1 for PASSWORD/ASK_USER, 0 otherwise.
 */
int tui_modal_needs_input(TuiModalKind kind);

#endif /* ECHO_TUI_DIALOGS_H */
