#ifndef ECHO_OPENCODE_ZEN_H
#define ECHO_OPENCODE_ZEN_H

#include "provider.h"

/* OpenCode Zen provider: thin wrapper over the OpenAI-compatible client
 * pointed at opencode.ai/zen/v1 with a Bearer token. Zen serves the
 * OpenAI chat/completions API at https://opencode.ai/zen/v1/chat/completions.
 * base_url: NULL -> https://opencode.ai/zen/v1. api_token is required by
 * Zen (issued at opencode.ai/auth); NULL/empty means the request goes out
 * unauthenticated (the service will reject with 401). The returned provider
 * is owned by the caller and freed via its destroy() callback. */
LLMProvider *opencode_zen_provider_create(const char *base_url,
                                          const char *api_token);

#endif
