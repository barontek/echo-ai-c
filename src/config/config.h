/*
 * config.h - INI-style config loading with section.key lookups and
 * [providers] token access.
 * Depends on: none beyond the C standard library (Conf is opaque).
 */

#ifndef ECHO_CONFIG_H
#define ECHO_CONFIG_H

typedef struct Conf Conf;

typedef struct {
    char *provider;
    char *token;
} ConfToken;

/**
 * conf_load - parse an INI-style file into a new Conf
 * @path: path of the file to read; must be non-NULL.
 *
 * Supports [section] headers, key=value lines (looked up as
 * "section.key"), blank lines and '#' comments, and continuation lines
 * that append to the previous entry's value. Malformed lines are silently
 * skipped, as are entries beyond the fixed 256-entry table. A file that
 * opens but contains no parseable keys yields an empty (zero-entry) Conf,
 * not NULL.
 *
 * Return: caller-owned Conf (free with conf_free()), or NULL when the
 * file cannot be opened or allocation fails. Thread-safe; no shared
 * state.
 */
Conf *conf_load(const char *path);

/**
 * conf_get - look up a key's value
 * @conf: Conf to search; must be non-NULL.
 * @key: full key, e.g. "safety.workspace"; must be non-NULL. Matching is
 *   exact and case-sensitive.
 *
 * Return: borrowed pointer to the value, owned by conf and valid until
 * conf_free(); NULL when the key is absent. Never fails; thread-safe.
 */
const char *conf_get(const Conf *conf, const char *key);

/**
 * conf_get_int - parse a key's value as an integer
 * @conf: Conf to search; must be non-NULL.
 * @key: key to look up (see conf_get()).
 * @def: value returned when the key is absent or not a valid integer.
 *
 * Return: parsed value, or def. Never fails; thread-safe.
 */
int conf_get_int(const Conf *conf, const char *key, int def);

/**
 * conf_free - release a Conf and every entry
 * @conf: Conf to release, or NULL (no-op).
 *
 * Frees all keys and values, then conf itself.
 *
 * Return: void. Thread-safe; no shared state.
 */
void conf_free(Conf *conf);

/**
 * conf_provider_tokens_alloc - collect every [providers] entry
 * @conf: Conf to scan; must be non-NULL.
 * @out: on success receives a caller-owned array of count ConfTokens;
 *   set to NULL when the section is absent or empty. Must be non-NULL.
 * @out_count: receives the array length. Must be non-NULL.
 *
 * Provider names are the key suffix after "providers."; a bare
 * "providers." key (no name) is skipped.
 *
 * Return: 0 on success (caller frees *out with conf_token_list_free()),
 * -1 on NULL arguments or allocation failure — in the failure case *out
 * is NULL and *out_count is 0. Thread-safe; no shared state.
 */
int conf_provider_tokens_alloc(const Conf *conf, ConfToken **out, int *out_count);

/**
 * conf_token_list_free - free an array from conf_provider_tokens_alloc()
 * @tokens: array to release, or NULL (no-op).
 * @count: number of entries in tokens.
 *
 * Frees each entry's provider/token strings and the array itself.
 *
 * Return: void.
 */
void conf_token_list_free(ConfToken *tokens, int count);

/**
 * conf_provider_token - look up a provider's token in [providers]
 * @conf: Conf to search; must be non-NULL.
 * @provider: provider name; must be non-NULL.
 *
 * "opencode_zen" is looked up under the "opencode" key so OpenCode Zen
 * and a future OpenCode Go provider (same key) can share one entry.
 *
 * Return: borrowed pointer to the token, owned by conf and valid until
 * conf_free(); NULL when the provider has no entry. Never fails;
 * thread-safe.
 */
const char *conf_provider_token(const Conf *conf, const char *provider);

#ifdef CONFIG_TEST
/**
 * config_test_set_alloc_fail - fail the Nth str_dup call
 * @nth_allocation: 1-based index of the str_dup call to make fail, or -1
 *   to disable failure injection.
 *
 * Test-only hook: makes the Nth str_dup inside config.c return NULL so
 * allocation-failure paths can be exercised deterministically. Only
 * compiled under CONFIG_TEST.
 *
 * Return: void.
 */
void config_test_set_alloc_fail(int nth_allocation);
#endif

#endif