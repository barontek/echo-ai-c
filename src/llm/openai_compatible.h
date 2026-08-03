#ifndef ECHO_OPENAI_COMPATIBLE_H
#define ECHO_OPENAI_COMPATIBLE_H

#include <stddef.h>

#include "provider.h"

/* OpenAI-compatible chat client (LM Studio, vLLM, llama.cpp server, ...).
 * base_url: endpoint, e.g. http://localhost:1234 (NULL -> default).
 * api_token: optional Bearer token sent as Authorization header; NULL/empty
 * means no auth. effort is a borrowed reasoning-effort hint: NULL or empty
 * means the API default; otherwise it must be one of "low", "medium",
 * "high", "max", "none" — anything else is rejected with NULL returned and
 * an error logged. When set, "reasoning_effort" is embedded in every
 * chat-completions request body. The returned provider is owned by the
 * caller and freed via its destroy() callback. */
LLMProvider *openai_compatible_provider_create(const char *base_url,
                                               const char *api_token,
                                               const char *effort);

/* Returns 1 when effort is NULL, empty, or one of the accepted reasoning
 * effort values ("low", "medium", "high", "max", "none"); 0 otherwise.
 * openai_compatible endpoints are not required to support xhigh, so it is
 * intentionally absent here. */
int openai_compatible_reasoning_effort_valid(const char *effort);

#ifdef OPENAI_COMPATIBLE_TEST
LLMResponse *openai_compatible_test_parse_stream(
    const char *input, void (*on_chunk)(const char *, void *), void *userdata);
LLMResponse *openai_compatible_test_stream_fragments(
    const char **fragments, size_t *lengths, int count,
    void (*on_chunk)(const char *, void *), void *userdata);
char *openai_compatible_test_build_url(const char *base_url);
char *openai_compatible_test_build_body(const char *model, const char *msgs_json,
                                        int stream, double temperature,
                                        const char *tools_json,
                                        const char *json_schema,
                                        int force_json_format,
                                        const char *effort);
#endif

#endif
