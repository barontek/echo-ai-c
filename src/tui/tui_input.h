/*
 * tui_input.h - single-line input editor model: growable buffer, cursor,
 * history with cap, and the editing operations the TUI binds to keys.
 * Byte-based model (UTF-8 codepoints are never split: all operations work
 * on whole bytes and the renderer clamps the cursor to a codepoint
 * boundary). No terminal I/O here — fully unit-testable.
 * Depends on: stdlib.
 */

#ifndef ECHO_TUI_INPUT_H
#define ECHO_TUI_INPUT_H

#include <stddef.h>

typedef struct TuiInput TuiInput;

/**
 * tui_input_create - allocate an input editor
 * @history_cap: maximum number of history entries retained; 0 disables
 *   history.
 *
 * Return: caller-owned TuiInput with an empty buffer, or NULL on
 * allocation failure. Release with tui_input_destroy().
 */
TuiInput *tui_input_create(size_t history_cap);

/**
 * tui_input_destroy - release an input editor
 * @in: editor to release, or NULL (no-op).
 *
 * Return: void.
 */
void tui_input_destroy(TuiInput *in);

/**
 * tui_input_insert - insert text at the cursor
 * @in: editor; non-NULL.
 * @text: bytes to insert, borrowed; NULL or empty is a no-op.
 *
 * The buffer grows on demand; on allocation failure the buffer and cursor
 * are unchanged and -1 is returned (no partial insertion).
 *
 * Return: 0 on success, -1 on allocation failure.
 */
int tui_input_insert(TuiInput *in, const char *text);

/**
 * tui_input_backspace - delete the byte before the cursor
 * @in: editor; non-NULL.
 *
 * Refuses to split a UTF-8 codepoint: when the byte before the cursor is a
 * continuation byte (0x80-0xBF), the whole codepoint is deleted.
 *
 * Return: number of bytes deleted (0 at the start of the buffer).
 */
int tui_input_backspace(TuiInput *in);

/**
 * tui_input_delete - delete the byte at the cursor
 * @in: editor; non-NULL.
 *
 * Same codepoint-atomicity rule as backspace, deleting forward.
 *
 * Return: number of bytes deleted (0 at the end of the buffer).
 */
int tui_input_delete(TuiInput *in);

/**
 * tui_input_move - move the cursor
 * @in: editor; non-NULL.
 * @steps: positive moves right, negative moves left. The cursor is clamped
 *   to the buffer and never lands inside a UTF-8 codepoint.
 *
 * Return: void.
 */
void tui_input_move(TuiInput *in, long steps);

/**
 * tui_input_home / tui_input_end - jump the cursor to start / end
 * @in: editor; non-NULL.
 *
 * Return: void.
 */
void tui_input_home(TuiInput *in);
void tui_input_end(TuiInput *in);

/**
 * tui_input_clear - empty the buffer and reset the cursor
 * @in: editor; non-NULL.
 *
 * History is untouched.
 *
 * Return: void.
 */
void tui_input_clear(TuiInput *in);

/**
 * tui_input_delete_word - delete the word before the cursor (Ctrl-W)
 * @in: editor; non-NULL.
 *
 * Deletes everything from the cursor back to (but not including) the last
 * space boundary.
 *
 * Return: number of bytes deleted.
 */
int tui_input_delete_word(TuiInput *in);

/**
 * tui_input_set_text - replace the whole buffer
 * @in: editor; non-NULL.
 * @text: bytes to install, borrowed; NULL clears the buffer.
 *
 * Cursor moves to the end. On allocation failure the previous content is
 * kept and -1 returned.
 *
 * Return: 0 on success, -1 on allocation failure.
 */
int tui_input_set_text(TuiInput *in, const char *text);

/**
 * tui_input_text - the current buffer contents
 * @in: editor; non-NULL.
 *
 * Return: borrowed, NUL-terminated buffer; never NULL (empty string when
 *   the buffer is empty). Valid until the next mutating call.
 */
const char *tui_input_text(const TuiInput *in);

/**
 * tui_input_len - byte length of the current buffer
 * @in: editor; non-NULL.
 *
 * Return: length in bytes.
 */
size_t tui_input_len(const TuiInput *in);

/**
 * tui_input_cursor - cursor position in bytes
 * @in: editor; non-NULL.
 *
 * Return: 0..tui_input_len().
 */
size_t tui_input_cursor(const TuiInput *in);

/**
 * tui_input_submit - commit the current line and push it to history
 * @in: editor; non-NULL.
 *
 * The caller owns the returned string. The buffer is cleared afterwards.
 * Empty lines are not pushed to history. On allocation failure the buffer
 * is kept untouched and NULL is returned.
 *
 * Return: caller-owned copy of the submitted line, or NULL on allocation
 *   failure (buffer preserved).
 */
char *tui_input_submit(TuiInput *in);

/**
 * tui_input_history_back / tui_input_history_forward - walk history
 * @in: editor; non-NULL.
 *
 * Moves the history cursor (away from the current edit) and installs the
 * target entry as the buffer, cursor at the end. Returns the installed
 * buffer (borrowed) or NULL at the edges of history.
 *
 * Return: borrowed buffer contents after the move, or NULL when there is
 *   no entry in that direction.
 */
const char *tui_input_history_back(TuiInput *in);
const char *tui_input_history_forward(TuiInput *in);

/**
 * tui_input_reset_history_walk - return to the live edit
 * @in: editor; non-NULL.
 *
 * Called after a submission or when the user edits while walking history;
 * further history moves start from the newest entry again.
 *
 * Return: void.
 */
void tui_input_reset_history_walk(TuiInput *in);

#endif /* ECHO_TUI_INPUT_H */
