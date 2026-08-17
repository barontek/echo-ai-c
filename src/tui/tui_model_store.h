/*
 * tui_model_store.h - persistent recent/favorite model lists
 * (JSON: {"recent":[...], "favorites":[...]}). Recent is most-recently-
 * used first, capped and deduped; favorites are user-toggled. Pure
 * file/JSON logic, fully unit-testable against temp files.
 * Depends on: stdlib, cJSON.
 */

#ifndef ECHO_TUI_MODEL_STORE_H
#define ECHO_TUI_MODEL_STORE_H

#include <stddef.h>

/**
 * tui_model_store_load - load recent and favorite model ids
 * @path: store file path; non-NULL.
 * @recent: receives a caller-owned array (most recent first); non-NULL.
 * @recent_count: receives the recent length; non-NULL.
 * @favorites: receives a caller-owned array; non-NULL.
 * @favorite_count: receives the favorites length; non-NULL.
 *
 * A missing file yields empty arrays (not an error). The caller frees
 * each string and both arrays.
 *
 * Return: 0 on success, -1 on allocation failure.
 */
int tui_model_store_load(const char *path, char ***recent, int *recent_count,
                         char ***favorites, int *favorite_count);

/**
 * tui_model_store_record_recent - mark a model as used
 * @path: store file path; non-NULL.
 * @id: model id (e.g. "provider/model"); non-NULL.
 * @cap: maximum number of recent entries kept.
 *
 * Moves @id to the front (dedup), trims to @cap, rewrites the file.
 * Existing favorites are preserved.
 *
 * Return: 0 on success, -1 on failure (store unchanged).
 */
int tui_model_store_record_recent(const char *path, const char *id, int cap);

/**
 * tui_model_store_toggle_favorite - flip a model's favorite state
 * @path: store file path; non-NULL.
 * @id: model id; non-NULL.
 * @now_favorite: receives 1 if favorite after the toggle, else 0; may be
 *   NULL.
 *
 * Return: 0 on success, -1 on failure (store unchanged).
 */
int tui_model_store_toggle_favorite(const char *path, const char *id,
                                    int *now_favorite);

#endif /* ECHO_TUI_MODEL_STORE_H */