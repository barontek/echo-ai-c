#ifndef ECHO_CONFIG_H
#define ECHO_CONFIG_H

typedef struct Conf Conf;

typedef struct {
    char *provider;
    char *token;
} ConfToken;

Conf *conf_load(const char *path);
const char *conf_get(const Conf *conf, const char *key);
int conf_get_int(const Conf *conf, const char *key, int def);
void conf_free(Conf *conf);

/* Returns all [providers] entries as an allocated array of ConfToken
 * (out_count = 0, *out = NULL when the section is absent or empty).
 * On success caller owns the array and frees it with conf_token_list_free.
 * Returns 0 on success, -1 on allocation failure (nothing returned). */
int conf_provider_tokens_alloc(const Conf *conf, ConfToken **out, int *out_count);
void conf_token_list_free(ConfToken *tokens, int count);

/* Returns the [providers] token for a provider name as a borrowed pointer
 * (owned by conf; NULL when absent). opencode_zen reads the shared
 * "providers.opencode" key so OpenCode Zen and a future OpenCode Go
 * provider (same key) can share one entry. */
const char *conf_provider_token(const Conf *conf, const char *provider);

#ifdef CONFIG_TEST
void config_test_set_alloc_fail(int nth_allocation);
#endif

#endif