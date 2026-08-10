/*
 * search_provider.h - Common vtable for the web-search backends (Brave,
 * DuckDuckGo, Tavily) installed as the registry's search provider.
 * Depends on: nothing beyond the C library (self-contained).
 */

#ifndef ECHO_SEARCH_PROVIDER_H
#define ECHO_SEARCH_PROVIDER_H

/**
 * SearchProvider - one web-search backend behind a shared vtable.
 *
 * Instances are heap-allocated; release them with destroy(), which frees
 * the instance, its owned name string, and its per-backend ctx.
 *
 * search() performs a live network request on every call and returns a
 * caller-owned heap string (free with free()): a JSON array of
 * {"title","url","snippet"} objects on success, a human-readable
 * "Error: ..." string on network/backend failure, "(no results)" when
 * the backend returned nothing, and NULL only on allocation failure.
 * Safe to call concurrently: each call uses its own CURL handle and ctx
 * holds immutable configuration.
 */
typedef struct SearchProvider SearchProvider;

struct SearchProvider {
    char *name; /* owned; backend name, e.g. "brave" */
    char *(*search)(SearchProvider *self, const char *query, int num_results); /* heap string, caller frees */
    void (*destroy)(SearchProvider *self); /* frees self plus owned fields and ctx */
    void *ctx; /* owned per-backend state; freed by destroy() */
};

/**
 * search_provider_create - instantiate a search backend by name
 * @provider_name: backend name: "brave", "duckduckgo", or "tavily";
 *   NULL is rejected silently.
 * @api_key: API key for brave/tavily, or NULL/empty to send requests
 *   without a key (the backend then rejects them at search time).
 *   Ignored for duckduckgo, which needs no key.
 *
 * Unknown provider names are rejected with an error logged.
 *
 * Return: caller-owned SearchProvider, released via its destroy()
 * callback, or NULL on unknown name or allocation failure. Thread-safe;
 * no shared state.
 */
SearchProvider *search_provider_create(const char *provider_name, const char *api_key);

#endif
