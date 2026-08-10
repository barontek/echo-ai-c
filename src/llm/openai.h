/*
 * openai.h - ChatGPT Codex provider (OAuth-only, no API-key path):
 * streaming chat, structured output, and model-catalog fetch.
 * Depends on: provider.h.
 */

#ifndef ECHO_OPENAI_H
#define ECHO_OPENAI_H

#include <stddef.h>

#include "provider.h"

typedef struct OpenAIOAuth OpenAIOAuth;

/* Return codes for openai_models_fetch_alloc(). */
enum {
    OPENAI_MODELS_OK = 0,          /* Catalog parsed; count may be zero. */
    OPENAI_MODELS_UNAVAILABLE = -1, /* Transport, 5xx, or parse failure. */
    OPENAI_MODELS_DENIED = -2      /* 4xx: bad credentials or no entitlement. */
};

/**
 * openai_provider_create - construct the OAuth-only Codex provider
 * @base_url: ignored by this provider (the Codex endpoint is fixed);
 *   accepted and dropped for API compatibility with the other providers.
 * @api_token: ignored by this provider (auth comes from the OAuth
 *   manager, never a token); accepted and dropped like base_url.
 * @effort: borrowed reasoning-effort hint, or NULL/empty for the API
 *   default; otherwise one of "low", "medium", "high", "xhigh", "max",
 *   "none". Any other value is rejected with NULL returned and an error
 *   logged (no silent fallback to a default).
 * @auth: borrowed OAuth manager; must outlive the returned provider.
 *
 * Return: caller-owned LLMProvider, or NULL when auth is NULL, effort is
 * invalid, or allocation fails. The caller must release it via its
 * destroy() callback. Safe to call concurrently: the provider keeps no
 * mutable shared state and each request uses its own CURL handle.
 */
LLMProvider *openai_provider_create(const char *base_url, const char *api_token,
                                     const char *effort, OpenAIOAuth *auth);

/**
 * openai_models_fetch_alloc - fetch the caller's visible Codex catalog
 * @auth: borrowed OAuth manager used for the request; must be signed in
 *   and attached to a session.
 * @models_out: receives a caller-owned array of model slugs, or NULL
 *   when the catalog is empty. Free with openai_models_free().
 * @count_out: receives the number of slugs in models_out (0 when empty).
 *
 * A 401 triggers one token refresh and one retry. Failure details are
 * logged, not returned.
 *
 * Return: OPENAI_MODELS_OK (0) when the catalog parsed; the count may be
 * zero, in which case models_out is NULL. OPENAI_MODELS_UNAVAILABLE (-1)
 * for transport, 5xx, parse, or invalid-argument failures — the caller
 * may use a fallback catalog. OPENAI_MODELS_DENIED (-2) for any 4xx —
 * bad credentials or no entitlement — the caller must not offer models.
 * On non-OK the outputs are NULL/0. Thread-safe: no shared state beyond
 * the mutex-protected OAuth manager.
 */
int openai_models_fetch_alloc(OpenAIOAuth *auth, char ***models_out,
                              size_t *count_out);

/**
 * openai_models_free - free a catalog from openai_models_fetch_alloc
 * @models: the array to free, or NULL for a no-op.
 * @count: number of strings in models.
 *
 * Return: void. Safe to call with a NULL array; thread-safe.
 */
void openai_models_free(char **models, size_t count);

/**
 * openai_reasoning_effort_valid - validate a Codex reasoning-effort value
 * @effort: value to validate; NULL and empty are accepted.
 *
 * This is the single validation used for both config-provided and
 * wire-provided effort strings.
 *
 * Return: 1 when effort is NULL, empty, or one of "low", "medium",
 * "high", "xhigh", "max", "none"; 0 otherwise. Never fails; thread-safe.
 */
int openai_reasoning_effort_valid(const char *effort);

#ifdef OPENAI_TEST
/**
 * openai_test_build_request_body - build a Codex request body
 * @messages: array of messages; may be NULL only when count is 0.
 * @count: number of messages (>= 0).
 * @model: model name string.
 * @temperature: sampling temperature; must be finite and in [0, 2].
 * @stream: 1 for a streaming request body, 0 otherwise.
 * @tools_json: pre-serialized OpenAI-style "tools" array JSON, or
 *   NULL/empty for none.
 * @json_schema: pre-serialized JSON schema, or NULL/empty for none.
 * @effort: reasoning-effort hint, or NULL/empty for the API default.
 *
 * Test-only hook mirroring the provider's body builder exactly; performs
 * no network I/O.
 *
 * Return: caller-owned JSON string, or NULL on invalid arguments (bad
 * temperature, invalid effort, malformed messages/tools) or allocation
 * failure. Free with free(). Thread-safe; no shared state.
 */
char *openai_test_build_request_body(Message *messages, int count,
                                     const char *model, double temperature,
                                     int stream, const char *tools_json,
                                     const char *json_schema,
                                     const char *effort);

/**
 * openai_test_parse_response - parse a complete non-streaming response
 * @raw: Codex Responses API JSON document (the body of a 2xx reply).
 *
 * Test-only hook for the buffered parse path; extracts content,
 * reasoning summary (into <think> blocks), and function calls.
 *
 * Return: caller-owned LLMResponse, or NULL on parse or validation
 * failure. Free with llm_response_free(). Thread-safe; no shared state.
 */
LLMResponse *openai_test_parse_response(const char *raw);

/**
 * openai_test_stream_fragments - parse an SSE stream split across fragments
 * @fragments: array of byte fragments that together form the SSE payload.
 * @lengths: per-fragment byte lengths, or NULL to strlen() each fragment.
 * @count: number of fragments (>= 0).
 * @on_chunk: callback fired once per content delta with the raw text
 *   (including <think> tags), or NULL to suppress. Called synchronously.
 * @userdata: opaque pointer forwarded to on_chunk unchanged.
 *
 * Test-only hook exercising the partial-line buffering path, i.e. the
 * same delivery shape curl produces when the network splits lines.
 *
 * Return: caller-owned LLMResponse (aggregated content and tool calls),
 * or NULL on invalid arguments, allocation, or stream validation failure.
 * Free with llm_response_free(). Thread-safe; no shared state.
 */
LLMResponse *openai_test_stream_fragments(
    const char **fragments, const size_t *lengths, int count,
    void (*on_chunk)(const char *, void *), void *userdata);

/**
 * openai_test_request_metadata - resolve the request URL, headers, timeout
 * @token: access token used to build the Authorization header.
 * @account: optional ChatGPT account id, or NULL for none.
 * @body: request body; only validated for size and non-NULL.
 * @timeout: request timeout in seconds (> 0).
 * @url_out: receives a caller-owned URL string; free with free().
 * @headers_out: receives a caller-owned newline-joined header list. It
 *   contains the Bearer token — treat as a secret; free with free().
 * @timeout_out: receives the timeout applied to the request.
 *
 * Test-only hook mirroring the live request setup; performs no transfer
 * (no network I/O).
 *
 * Return: 0 on success, -1 on invalid arguments or setup failure with
 * all outputs left NULL/0. Thread-safe; no shared state.
 */
int openai_test_request_metadata(const char *token, const char *account,
                                 const char *body, int timeout,
                                 char **url_out, char **headers_out,
                                 long *timeout_out);

/**
 * openai_test_refresh_after_401 - refresh a token after a 401
 * @auth: borrowed OAuth manager.
 * @rejected_token: the access token that produced the 401.
 * @access_token: receives a caller-owned refreshed token (secret);
 *   free with free().
 * @account_id: receives a caller-owned account id, or NULL when unknown;
 *   free with free().
 *
 * Test-only hook wrapping the live 401-recovery path; performs real
 * network I/O via the OAuth manager.
 *
 * Return: 0 on success, -1 when arguments are invalid or the refresh
 * fails (outputs left NULL). Thread-safe; the OAuth manager is
 * mutex-protected.
 */
int openai_test_refresh_after_401(OpenAIOAuth *auth,
                                  const char *rejected_token,
                                  char **access_token, char **account_id);

/**
 * openai_test_parse_models - parse a Codex models-catalog response
 * @raw: catalog response JSON body.
 * @models_out: receives a caller-owned array of list-visible model slugs,
 *   or NULL when the catalog is empty. Free with openai_models_free().
 * @count_out: receives the number of slugs (0 when empty).
 *
 * Test-only hook wrapping the catalog parser used by
 * openai_models_fetch_alloc(); performs no network I/O.
 *
 * Return: 0 on success, -1 on invalid arguments or parse failure with
 * the outputs left NULL/0. Thread-safe; no shared state.
 */
int openai_test_parse_models(const char *raw, char ***models_out,
                             size_t *count_out);
#endif

#endif
