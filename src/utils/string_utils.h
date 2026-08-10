/*
 * string_utils.h - common string helpers: trimming, duplication,
 * splitting, JSON sanitizing, and truncation. Depends on: none.
 */

#ifndef ECHO_STRING_UTILS_H
#define ECHO_STRING_UTILS_H

/**
 * str_trim - strip leading and trailing whitespace in place
 * @str: NUL-terminated buffer to trim in place; NULL yields NULL.
 *
 * Leading whitespace is memmoved away, so the returned pointer always
 * equals the passed buffer (an interior pointer is never returned).
 * The input buffer is mutated and must be writable.
 *
 * Return: the passed @str pointer (points into the caller's buffer,
 * which the caller still owns and frees). NULL only when @str is NULL.
 * Never fails; thread-safe as long as the buffer is not shared.
 */
char *str_trim(char *str);

/**
 * str_dup - duplicate a string into a fresh buffer
 * @str: NUL-terminated input.
 *
 * Duplicate @str into a freshly malloc'd NUL-terminated buffer the
 * caller must free.
 *
 * Return: NULL when @str is NULL (a NULL-tolerant no-op, not an error)
 * or when malloc fails (true OOM, distinct from the legitimate-NULL
 * case above). Callers that distinguish the two cases (e.g. parsing
 * optional JSON keys where a missing key legitimately yields NULL) can
 * safely pass the NULL through; callers that treat NULL as OOM (e.g.
 * session_create's title, save_session_core's serialize OOM path) MUST
 * check the return — the F1/F3 split is a known design point in this
 * codebase. Thread-safe; no shared state.
 */
char *str_dup(const char *str);

/**
 * str_append - append text onto the end of a heap string
 * @target: pointer to the string to grow; must be non-NULL. The pointee
 *   may be NULL (an empty string) — the result is then a fresh copy of
 *   @value. Ownership of the pointee passes through: the caller's
 *   pointer is updated to the new allocation on success.
 * @value: NUL-terminated text to append; must be non-NULL.
 *
 * Grows the string in place with realloc, keeping it NUL-terminated.
 * Refuses an append whose combined length would overflow size_t.
 *
 * Return: 0 on success (the caller's *target now owns the new string),
 * -1 on NULL arguments or allocation failure (on failure *target is
 * unchanged and still owned by the caller). Thread-safe on distinct
 * strings; no shared state.
 */
int str_append(char **target, const char *value);

/**
 * strlcpy - bounded copy with guaranteed NUL termination
 * @dst: destination buffer, must be non-NULL and writable.
 * @src: NUL-terminated source; must be non-NULL.
 * @size: capacity of @dst in bytes, including room for the NUL.
 *
 * POSIX strlcpy semantics: copies at most @size - 1 bytes and always
 * NUL-terminates @dst. Unlike strncpy it never pads with NULs and never
 * leaves the buffer unterminated.
 *
 * On Apple these are provided by libSystem (under _FORTIFY_SOURCE they
 * become __builtin___strlcat_chk macros, which would collide with a
 * definition here), so the declarations and definitions below are
 * non-Apple only. Semantics are identical either way.
 *
 * Return: the length of @src (the would-be copied length). Callers must
 * check for truncation: a return >= @size means the copy was cut short.
 * Never fails; thread-safe as long as the buffers are not shared.
 */
#if !defined(__APPLE__)
size_t strlcpy(char *dst, const char *src, size_t size);

/**
 * strlcat - bounded append with guaranteed NUL termination
 * @dst: NUL-terminated destination buffer, must be non-NULL and writable.
 * @src: NUL-terminated source; must be non-NULL.
 * @size: capacity of @dst in bytes, including room for the NUL.
 *
 * POSIX strlcat semantics: appends at most @size - strlen(dst) - 1 bytes
 * and always NUL-terminates @dst.
 *
 * Return: the would-be total length (strlen(dst) + strlen(src)). Callers
 * must check for truncation: a return >= @size means the append was cut
 * short. Never fails; thread-safe as long as the buffers are not shared.
 */
size_t strlcat(char *dst, const char *src, size_t size);
#endif

/**
 * str_starts_with - prefix test
 * @str: string to test; NULL yields 0.
 * @prefix: prefix to look for; NULL yields 0. The empty string
 *   matches any @str.
 *
 * Return: 1 when @str begins with @prefix, 0 otherwise. Never fails;
 * thread-safe.
 */
int str_starts_with(const char *str, const char *prefix);

/**
 * str_ends_with - suffix test
 * @str: string to test; NULL yields 0.
 * @suffix: suffix to look for; NULL yields 0. The empty string matches
 *   any @str.
 *
 * Return: 1 when @str ends with @suffix, 0 otherwise. Never fails;
 * thread-safe.
 */
int str_ends_with(const char *str, const char *suffix);

typedef struct {
    char **items;
    int count;
} StrArray;

/**
 * str_split - split a string on a delimiter into fresh copies
 * @str: NUL-terminated string to split; NULL yields {NULL, 0}.
 * @delimiter: byte to split on.
 *
 * Every segment is a freshly malloc'd copy, including empty segments
 * (delimiter runs and leading/trailing delimiters produce "" items).
 * On allocation failure the result degrades silently: a failed segment
 * copy is skipped, and a failed items-array grow frees everything and
 * returns {NULL, 0}.
 *
 * Return: StrArray of @count caller-owned item strings. Release every
 * item and the array with str_array_free(). Thread-safe; no shared
 * state; the input is only read.
 */
StrArray str_split(const char *str, char delimiter);

/**
 * str_array_free - free a StrArray and all its items
 * @arr: array to free; NULL, or a {NULL, 0} array, is a safe no-op.
 *
 * Resets @arr to {NULL, 0} after freeing so it can be reused or freed
 * again safely.
 *
 * Return: void; never fails.
 */
void str_array_free(StrArray *arr);

/**
 * sanitize_json_dup - normalize an LLM-produced JSON fragment
 * @str: NUL-terminated input; NULL yields NULL.
 *
 * Removes surrounding whitespace, optional ``` (or ```json) markdown
 * fences, whitespace/newlines outside string literals, and trailing
 * commas that precede a closing bracket.
 *
 * Return: freshly malloc'd NUL-terminated string owned by the caller
 * (free with free()), or NULL on allocation failure. Thread-safe; no
 * shared state.
 */
char *sanitize_json_dup(const char *str);

/**
 * str_truncate_ellipsis_dup - hard-truncate a string with an omission marker
 * @text: NUL-terminated input; NULL yields NULL.
 * @max_chars: maximum byte length of the result.
 *
 * Text at most @max_chars bytes is duplicated unchanged. Longer text
 * keeps its head and appends "[... truncated, N chars omitted ...]"
 * when the marker fits, otherwise it is hard-cut to @max_chars.
 *
 * Return: freshly malloc'd NUL-terminated string owned by the caller
 * (free with free()), or NULL on allocation failure. Thread-safe; no
 * shared state.
 */
char *str_truncate_ellipsis_dup(const char *text, size_t max_chars);

#endif
