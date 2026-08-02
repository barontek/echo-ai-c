#ifndef ECHO_OPENAI_H
#define ECHO_OPENAI_H

#include "provider.h"

/* Real OpenAI provider: thin wrapper over the OpenAI-compatible client
 * pointed at api.openai.com with a Bearer token.
 * base_url: NULL -> https://api.openai.com. api_token is required by the
 * OpenAI service; NULL/empty means the request goes out unauthenticated
 * (the service will reject with 401). The returned provider is owned by
 * the caller and freed via its destroy() callback. */
LLMProvider *openai_provider_create(const char *base_url, const char *api_token);

#endif
