#ifndef ECHO_OPENAI_COMPATIBLE_H
#define ECHO_OPENAI_COMPATIBLE_H

#include "provider.h"

/* OpenAI-compatible chat client (LM Studio, vLLM, llama.cpp server, ...).
 * base_url: endpoint, e.g. http://localhost:1234 (NULL -> default).
 * api_token: optional Bearer token sent as Authorization header; NULL/empty
 * means no auth. The returned provider is owned by the caller and freed
 * via its destroy() callback. */
LLMProvider *openai_compatible_provider_create(const char *base_url,
                                               const char *api_token);

#ifdef OPENAI_COMPATIBLE_TEST
LLMResponse *openai_compatible_test_parse_stream(
    const char *input, void (*on_chunk)(const char *, void *), void *userdata);
#endif

#endif
