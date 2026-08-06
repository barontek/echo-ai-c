#ifndef ECHO_STRING_UTILS_H
#define ECHO_STRING_UTILS_H

char *str_trim(char *str);

/* Duplicate `str` into a freshly malloc'd NUL-terminated buffer the caller
 * must `free`. Returns NULL when:
 *   - `str` is NULL (the function is a NULL-tolerant no-op, not an error)
 *   - malloc fails (true OOM, distinct from the legitimate-NULL case above)
 * Callers that distinguish the two cases (e.g. parsing optional JSON keys
 * where a missing key legitimately yields NULL) can safely pass the NULL
 * through; callers that treat NULL as OOM (e.g. `session_create`'s title,
 * `save_session_core`'s serialize OOM path) MUST check the return — the
 * F1/F3 split is a known design point in this codebase. */
char *str_dup(const char *str);
int str_starts_with(const char *str, const char *prefix);
int str_ends_with(const char *str, const char *suffix);

typedef struct {
    char **items;
    int count;
} StrArray;

StrArray str_split(const char *str, char delimiter);
void str_array_free(StrArray *arr);

char *sanitize_json(const char *str);

/* Hard-truncate `text` to at most `max_chars` bytes, appending a
 * "[... truncated, N chars omitted ...]" marker when anything was cut.
 * Returns a freshly malloc'd NUL-terminated string the caller must free;
 * returns NULL only on allocation failure. NULL input yields NULL. */
char *str_truncate_ellipsis(const char *text, size_t max_chars);

#endif
