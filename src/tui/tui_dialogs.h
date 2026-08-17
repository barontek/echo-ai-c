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
    TUI_MODAL_NOTICE,     /* any key dismisses (unlock errors etc.) */
    TUI_MODAL_PICKER,     /* type-to-filter list; Enter selects, Esc cancels */
    TUI_MODAL_CONFIRM     /* y = yes, n/Esc = no (selection 1/0 on close) */
} TuiModalKind;

/* Picker navigation sentinels for tui_modal_dispatch_key(): negative so
 * they can never collide with a real codepoint or control byte. They must
 * also never equal -1, which the UI uses as "no key" (handle_key drops
 * key == -1 before dispatching). Ctrl-P (16) and Ctrl-N (14) are also
 * accepted as up/down by the dispatcher. */
enum {
    TUI_PICKER_KEY_UP = -101,
    TUI_PICKER_KEY_DOWN = -102,
    TUI_PICKER_KEY_PGUP = -103,
    TUI_PICKER_KEY_PGDOWN = -104
};

/* What the UI loop must do after a key was dispatched to the modal. */
typedef enum {
    TUI_MODAL_ACTION_NONE,    /* modal still open; keep rendering it */
    TUI_MODAL_ACTION_ANSWERED, /* answer written; close the modal (NONE) */
    TUI_MODAL_ACTION_QUIT      /* confirm-quit accepted; exit the app */
} TuiModalAction;

typedef struct {
    TuiModalKind kind;
    char *title;   /* owned */
    char *body;    /* owned; question text / tool args / newline-joined items */
    TuiEvent *event; /* round-trip event to answer; owned by caller */
    int event_result; /* approval result to write when answering */
    TuiInput *input;  /* password / answer / picker-filter buffer (owned) */
    /* TUI_MODAL_PICKER state (all owned, unused otherwise) */
    char **items;      /* every item offered */
    int item_count;
    int *visible;      /* items[] indices passing the current filter */
    int visible_count;
    int cursor;        /* row into visible[] */
    int top;           /* first visible[] row shown (scroll offset) */
    int window;        /* viewport rows (set by the renderer) */
    int selection;     /* selected items[] index after Enter, or -1 */
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
 * Return: 1 for PASSWORD/ASK_USER (and PICKER, whose input is the
 * type-to-filter buffer), 0 otherwise.
 */
int tui_modal_needs_input(TuiModalKind kind);

/* ---- TUI_MODAL_PICKER accessors (all non-NULL @m only) ---- */

/**
 * tui_modal_picker_item_count - number of items in the picker
 * @m: picker modal; non-NULL.
 *
 * Return: the count of items offered before any filtering.
 */
int tui_modal_picker_item_count(const TuiModal *m);

/**
 * tui_modal_picker_visible_count - items matching the current filter
 * @m: picker modal; non-NULL.
 *
 * Return: the number of rows the renderer may draw.
 */
int tui_modal_picker_visible_count(const TuiModal *m);

/**
 * tui_modal_picker_visible_at - the item text at a visible row
 * @m: picker modal; non-NULL.
 * @row: row into the visible list, 0-based.
 *
 * Return: borrowed item string, or NULL when @row is out of range.
 */
const char *tui_modal_picker_visible_at(const TuiModal *m, int row);

/**
 * tui_modal_picker_cursor / tui_modal_picker_top - list viewport
 * @m: picker modal; non-NULL.
 *
 * cursor is the selected visible row; top is the first visible row the
 * renderer should draw (scrolling window of tui_modal_picker_set_window
 * rows).
 *
 * Return: row index.
 */
int tui_modal_picker_cursor(const TuiModal *m);
int tui_modal_picker_top(const TuiModal *m);

/**
 * tui_modal_picker_set_window - viewport height for scrolling
 * @m: picker modal; non-NULL.
 * @rows: number of visible rows the renderer can draw (>= 1).
 *
 * Clamps the scroll offset so the cursor stays inside the window.
 *
 * Return: void.
 */
void tui_modal_picker_set_window(TuiModal *m, int rows);

/**
 * tui_modal_picker_selected - the item committed by Enter
 * @m: picker modal; non-NULL.
 *
 * Only meaningful after tui_modal_dispatch_key() returned ANSWERED with
 * Enter; NULL for a cancel (Esc) or an empty list.
 *
 * Return: borrowed pointer to the selected item string, or NULL.
 */
const char *tui_modal_picker_selected(const TuiModal *m);

#endif /* ECHO_TUI_DIALOGS_H */
