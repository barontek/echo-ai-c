/*
 * tui_session_store.h - persistent session pins (JSON: {"pinned":[...]})
 * and quick-switch slot resolution: slot N maps to a pinned session
 * first, then to the Nth remaining recent session. Pure file/JSON logic,
 * fully unit-testable against temp files. Depends on: stdlib, cJSON.
 */

#ifndef ECHO_TUI_SESSION_STORE_H
#define ECHO_TUI_SESSION_STORE_H

#include <stddef.h>

/**
 * tui_session_store_load_pins - load the pinned session ids
 * @path: pins file path; non-NULL.
 * @out: receives a caller-owned array of pinned ids (pin order, newest
 *   pin first); must be non-NULL.
 * @out_count: receives the array length; must be non-NULL.
 *
 * A missing or empty file yields count 0 (not an error). Malformed lines
 * are skipped. The caller frees each string and the array.
 *
 * Return: 0 on success, -1 on allocation failure.
 */
int tui_session_store_load_pins(const char *path, char ***out, int *out_count);

/**
 * tui_session_store_save_pins - persist the pinned session ids
 * @path: pins file path; non-NULL.
 * @ids: array of @count borrowed ids; may be NULL when @count is 0.
 * @count: number of pins.
 *
 * Writes {"pinned":[...]} atomically-ish (temp write + rename is the
 * caller's choice of filesystem; this module writes the file directly).
 *
 * Return: 0 on success, -1 on I/O or allocation failure.
 */
int tui_session_store_save_pins(const char *path, const char *const *ids,
                                int count);

/**
 * tui_session_store_toggle_pin - flip a session's pinned state
 * @path: pins file path; non-NULL.
 * @id: session id to toggle; non-NULL.
 * @now_pinned: receives 1 if the session is pinned after the toggle,
 *   0 otherwise; may be NULL.
 *
 * A newly pinned id goes to the front (most recent pin). The file is
 * rewritten on every toggle.
 *
 * Return: 0 on success, -1 on failure (pins unchanged).
 */
int tui_session_store_toggle_pin(const char *path, const char *id,
                                 int *now_pinned);

/**
 * tui_session_store_resolve_slot - map a quick-switch slot to a session
 * @pinned: pinned ids in pin order; may be NULL when @pinned_count is 0.
 * @pinned_count: number of pinned ids.
 * @all: every session id, most-recent first; may be NULL when @all_count
 *   is 0.
 * @all_count: number of all sessions.
 * @slot: 1-based slot number.
 * @out: caller-owned buffer for the resolved id; non-NULL.
 * @out_cap: capacity of @out; must be >= 1.
 *
 * Slot N resolves to the Nth entry of the ordered list: pinned ids first
 * (skipping any not present in @all), then every non-pinned id in @all's
 * order. This matches opencode's "<leader>1-9 quick switch" semantics.
 *
 * Return: 1 when a session was written to @out, 0 when @slot is out of
 *   range.
 */
int tui_session_store_resolve_slot(const char *const *pinned, int pinned_count,
                                   const char *const *all, int all_count,
                                   int slot, char *out, size_t out_cap);

#endif /* ECHO_TUI_SESSION_STORE_H */