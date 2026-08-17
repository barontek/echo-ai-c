/*
 * tui_prompt_store.h - persistent prompt history and stash, JSONL files
 * under the state directory. History is capped and consecutive-deduped;
 * the stash is a capped LIFO list. Pure file/JSON logic, fully
 * unit-testable against temp files. Depends on: stdlib, cJSON.
 */

#ifndef ECHO_TUI_PROMPT_STORE_H
#define ECHO_TUI_PROMPT_STORE_H

#include <stddef.h>

/**
 * tui_prompt_store_history_append - record a submitted prompt
 * @path: history file path; non-NULL.
 * @input: prompt text; non-NULL.
 * @mode: "shell" or NULL/"normal".
 * @max_entries: cap; entries past the newest @max_entries are dropped.
 *
 * Appends a JSON line; a consecutive duplicate of the newest entry is
 * ignored. The file is rewritten (append + trim) so the cap is exact.
 *
 * Return: 0 on success, -1 on I/O or allocation failure.
 */
int tui_prompt_store_history_append(const char *path, const char *input,
                                    const char *mode, int max_entries);

/**
 * tui_prompt_store_history_load - load every stored prompt
 * @path: history file path; non-NULL.
 * @out: receives a caller-owned array of borrowed-into-alloc strings
 *   (newest LAST, matching tui_input history order); must be non-NULL.
 * @out_count: receives the array length; must be non-NULL.
 *
 * Malformed lines are skipped; a missing file yields count 0 (not an
 * error). The caller frees each string and the array.
 *
 * Return: 0 on success, -1 on allocation failure (*out NULL, count 0).
 */
int tui_prompt_store_history_load(const char *path, char ***out, int *out_count);

/**
 * tui_prompt_store_stash_push - stash the current draft
 * @path: stash file path; non-NULL.
 * @input: draft text; non-NULL.
 * @max_entries: cap.
 *
 * Return: 0 on success, -1 on failure.
 */
int tui_prompt_store_stash_push(const char *path, const char *input,
                                int max_entries);

/**
 * tui_prompt_store_stash_pop - remove and return the newest stashed draft
 * @path: stash file path; non-NULL.
 * @out: receives a caller-owned copy of the popped input; must be non-NULL.
 *
 * Return: 0 on success (out set), -1 on failure or an empty stash (out
 *   set to NULL).
 */
int tui_prompt_store_stash_pop(const char *path, char **out);

/**
 * tui_prompt_store_stash_list - list stashed drafts, newest first
 * @path: stash file path; non-NULL.
 * @out: receives a caller-owned array of strings (newest FIRST); non-NULL.
 * @out_count: receives the array length; non-NULL.
 *
 * Return: 0 on success, -1 on allocation failure.
 */
int tui_prompt_store_stash_list(const char *path, char ***out, int *out_count);

/**
 * tui_prompt_store_stash_remove - drop the stash entry at an index
 * @path: stash file path; non-NULL.
 * @index: 0-based index into the newest-first stash list.
 *
 * Return: 0 on success, -1 when the index is out of range or I/O fails.
 */
int tui_prompt_store_stash_remove(const char *path, int index);

#endif /* ECHO_TUI_PROMPT_STORE_H */