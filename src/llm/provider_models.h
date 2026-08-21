/*
 * provider_models.h - shared live model-list fetcher used by both the web
 * server's GET /api/models handler and the TUI worker's model picker, so
 * the two frontends never drift on endpoint/path/JSON-shape logic.
 * Depends on: libcurl, cJSON, factory.h, openai.h, openai_oauth.h.
 */

#ifndef ECHO_PROVIDER_MODELS_H
#define ECHO_PROVIDER_MODELS_H

#include <stddef.h>

typedef struct OpenAIOAuth OpenAIOAuth;

/* Return codes for provider_models_fetch_alloc(). */
enum {
    PROVIDER_MODELS_OK = 0,           /* Catalog parsed; count may be zero. */
    PROVIDER_MODELS_UNAVAILABLE = -1, /* Transport, 5xx, parse, or unknown provider. */
    PROVIDER_MODELS_DENIED = -2       /* 4xx: bad credentials or no entitlement. */
};

/**
 * provider_models_fetch_alloc - fetch the live model list for a provider
 * @provider: canonical provider name ("ollama", "openai",
 *   "openai_compatible", "opencode_zen", "opencode_go").
 * @base_url: endpoint base, or NULL for the provider's canonical default
 *   (provider_default_base_url). Ignored for "openai" (its endpoint is
 *   fixed inside the OAuth catalog fetch).
 * @api_token: Bearer token for openai_compatible/opencode_*; NULL/empty
 *   for none. Ignored for "openai" (OAuth-only).
 * @oauth: borrowed OAuth manager used for the "openai" provider, or NULL.
 * @models_out: receives a caller-owned array of model name strings, or
 *   NULL when the catalog is empty. Free with provider_models_free().
 * @count_out: receives the number of entries in models_out (0 when empty).
 *
 * Per-provider transport: ollama GET /api/tags; openai_compatible GET
 * /v1/models; opencode_zen/go GET /models (their base_url already ends in
 * /v1); openai via the OAuth catalog fetch (openai_models_fetch_alloc).
 *
 * Return: PROVIDER_MODELS_OK (0) when a list was obtained; count may be
 * zero (a signed-out OpenAI account or an empty remote catalog are
 * successes with an empty list). PROVIDER_MODELS_UNAVAILABLE (-1) for
 * transport, 5xx, parse, or unknown-provider failures — the caller may
 * fall back. PROVIDER_MODELS_DENIED (-2) only for the OpenAI 4xx path —
 * the caller must not offer models. On non-OK outputs are NULL/0.
 * Thread-safe: no shared state beyond the mutex-protected OAuth manager.
 */
int provider_models_fetch_alloc(const char *provider, const char *base_url,
                                const char *api_token, OpenAIOAuth *oauth,
                                char ***models_out, size_t *count_out);

/**
 * provider_models_free - release an array from provider_models_fetch_alloc
 * @models: the array to free, or NULL for a no-op.
 * @count: number of strings in models.
 *
 * Return: void. Safe to call with a NULL array and count 0.
 */
void provider_models_free(char **models, size_t count);

#ifdef PROVIDER_MODELS_TEST
/**
 * provider_models_parse_test - parse a models JSON document (no I/O)
 * @raw: JSON body (e.g. an ollama /api/tags or OpenAI-compatible
 *   /models response).
 * @list_key: the array field name ("models" for ollama, "data" for
 *   OpenAI-compatible endpoints).
 * @name_key: the per-entry field holding the model name ("name" for
 *   ollama, "id" for OpenAI-compatible endpoints).
 * @models_out: receives a caller-owned array of names, or NULL when the
 *   catalog is empty or unparseable. Free with provider_models_free().
 * @count_out: receives the number of entries (0 when empty).
 *
 * Test-only hook exposing the pure parser; performs no network I/O.
 *
 * Return: 0 on success (a shaped-but-empty document is a success with an
 *   empty list; a malformed document is treated as empty), -1 on NULL
 *   arguments. Thread-safe; no shared state.
 */
int provider_models_parse_test(const char *raw, const char *list_key,
                               const char *name_key,
                               char ***models_out, size_t *count_out);

/**
 * provider_models_test_set_strdup_fail / _strdup_calls - force and count
 * allocation failures in the parse path (fault injection).
 * @nth: 1-based index of the str_dup call to fail, or -1 to disable.
 *
 * provider_models_test_set_strdup_fail resets the call counter and fails
 * the Nth str_dup. provider_models_test_strdup_calls returns the current
 * call count (for asserting the parse stopped at the failure point).
 * Test-only; no-op semantics in production (not compiled).
 */
void provider_models_test_set_strdup_fail(int nth);
int provider_models_test_strdup_calls(void);
#endif

#endif /* ECHO_PROVIDER_MODELS_H */
