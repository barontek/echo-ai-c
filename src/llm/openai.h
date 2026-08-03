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

/* Creates the OAuth-only ChatGPT Codex Responses provider. base_url and
 * api_token are intentionally ignored; auth is borrowed and must outlive the
 * returned provider. effort is a borrowed reasoning-effort hint: NULL or
 * empty means the API default; otherwise it must be one of "low", "medium",
 * "high", "xhigh", "max", "none" — anything else is rejected with NULL
 * returned and an error logged (no silent fallback to a default). Returns
 * NULL when auth is NULL, effort is invalid, or allocation fails. The caller
 * owns the result and destroys it through destroy(). */
LLMProvider *openai_provider_create(const char *base_url, const char *api_token,
                                     const char *effort, OpenAIOAuth *auth);

/* Fetches the caller's visible Codex catalog into a caller-owned string array.
 * Returns OPENAI_MODELS_OK, OPENAI_MODELS_UNAVAILABLE (caller may use a
 * fallback catalog), or OPENAI_MODELS_DENIED (caller must not offer models). */
int openai_models_fetch_alloc(OpenAIOAuth *auth, char ***models_out,
                              size_t *count_out);

/* Frees a catalog returned by openai_models_fetch_alloc(). */
void openai_models_free(char **models, size_t count);

/* Returns 1 when effort is NULL, empty, or one of the accepted reasoning
 * effort values ("low", "medium", "high", "xhigh", "max", "none"); 0
 * otherwise. This is the single validation used for both config-provided
 * and wire-provided effort strings. */
int openai_reasoning_effort_valid(const char *effort);

#ifdef OPENAI_TEST
/* Test hooks return caller-owned values and never perform network I/O. */
char *openai_test_build_request_body(Message *messages, int count,
                                     const char *model, double temperature,
                                     int stream, const char *tools_json,
                                     const char *json_schema,
                                     const char *effort);
LLMResponse *openai_test_parse_response(const char *raw);
LLMResponse *openai_test_stream_fragments(
    const char **fragments, const size_t *lengths, int count,
    void (*on_chunk)(const char *, void *), void *userdata);
int openai_test_request_metadata(const char *token, const char *account,
                                 const char *body, int timeout,
                                 char **url_out, char **headers_out,
                                 long *timeout_out);
int openai_test_refresh_after_401(OpenAIOAuth *auth,
                                  const char *rejected_token,
                                  char **access_token, char **account_id);
int openai_test_parse_models(const char *raw, char ***models_out,
                             size_t *count_out);
#endif

#endif
