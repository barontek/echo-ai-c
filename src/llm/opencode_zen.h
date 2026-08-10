/*
 * opencode_zen.h - OpenCode Zen provider: the OpenAI-compatible client
 * pointed at opencode.ai/zen/v1 with a Bearer token. Depends on:
 * provider.h.
 */

#ifndef ECHO_OPENCODE_ZEN_H
#define ECHO_OPENCODE_ZEN_H

#include "provider.h"

/**
 * opencode_zen_provider_create - construct the OpenCode Zen provider
 * @base_url: endpoint base, e.g. http://localhost:1234; NULL uses
 *   https://opencode.ai/zen/v1. Borrowed for the duration of the call;
 *   the provider keeps its own copy.
 * @api_token: Bearer token required by Zen (issued at opencode.ai/auth);
 *   NULL/empty means the request goes out unauthenticated and the
 *   service will reject it with 401. Borrowed; the provider keeps its
 *   own copy.
 * @effort: borrowed reasoning-effort hint accepted exactly as
 *   openai_compatible accepts it ("low"/"medium"/"high"/"max"/"none"),
 *   or NULL/empty for the API default. Any other value is rejected.
 *
 * Thin wrapper: delegates to openai_compatible_provider_create() with
 * the Zen default endpoint, so it shares the OpenAI-compatible client's
 * validation and request semantics.
 *
 * Return: caller-owned LLMProvider, or NULL on invalid effort or
 * allocation failure. The caller must release it via its destroy()
 * callback. Safe to call concurrently: the provider keeps no mutable
 * shared state and each request uses its own CURL handle.
 */
LLMProvider *opencode_zen_provider_create(const char *base_url,
                                          const char *api_token,
                                          const char *effort);

#endif
