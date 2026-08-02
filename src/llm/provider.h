#ifndef ECHO_PROVIDER_H
#define ECHO_PROVIDER_H

#include "../agent/message.h"

typedef struct LLMProvider LLMProvider;

struct LLMProvider {
    LLMResponse *(*chat)(LLMProvider *self, Message *messages, int count,
                         const char *model, double temperature, int timeout,
                         const char *tools_json);
    LLMResponse *(*chat_streaming)(LLMProvider *self, Message *messages, int count,
                                   const char *model, double temperature, int timeout,
                                   void (*on_chunk)(const char *chunk, void *userdata),
                                   void *userdata,
                                   const char *tools_json);
    LLMResponse *(*extract_structured)(LLMProvider *self, Message *messages, int count,
                                        const char *model, double temperature, int timeout,
                                        const char *json_schema);
    void (*destroy)(LLMProvider *self);
    void *ctx;
};

LLMProvider *get_provider(const char *name, const char *model,
                          const char *base_url, const char *api_token,
                          int num_ctx, int keep_alive_secs);

/* Returns the canonical list of providers get_provider can construct,
 * as a static array (caller must not free). Aliases like "lmstudio"
 * and unimplemented names like "anthropic" are not listed. */
const char *const *provider_names_available(int *count);

/* Returns the default base URL for a provider name (the same fallback
 * each provider's _create uses when given NULL), or NULL for unknown
 * names. Caller must not free. Centralizes the mapping so startup
 * (main.c) and mid-session switches (routes_ws.c) resolve the same URL. */
const char *provider_default_base_url(const char *name);

#endif
