#ifndef ECHO_PROVIDER_H
#define ECHO_PROVIDER_H

#include "../agent/message.h"

typedef struct OpenAIOAuth OpenAIOAuth;

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

/* effort: reasoning-effort hint for providers that support it ("low"/
 * "medium"/"high", plus OpenAI's "minimal"). NULL or empty means the
 * provider's default. Providers that don't support effort ignore it.
 * The pointer is borrowed for the duration of the call. */
LLMProvider *get_provider(const char *name, const char *model,
                           const char *base_url, const char *api_token,
                           int num_ctx, int keep_alive_secs,
                           const char *effort);

LLMProvider *get_provider_with_auth(const char *name, const char *model,
                                    const char *base_url, const char *api_token,
                                    int num_ctx, int keep_alive_secs,
                                    const char *effort,
                                    OpenAIOAuth *openai_auth);

/* Returns the canonical list of providers get_provider can construct,
 * as a static array (caller must not free). Aliases like "lmstudio"
 * and unimplemented names like "anthropic" are not listed. */
const char *const *provider_names_available(int *count);

/* Returns the default base URL for a provider name (the same fallback
 * each provider's _create uses when given NULL), or NULL for unknown
 * names. Caller must not free. Centralizes the mapping so startup
 * (main.c) and mid-session switches (routes_ws.c) resolve the same URL. */
const char *provider_default_base_url(const char *name);

/* Returns 1 when the named provider accepts a reasoning-effort hint
 * (currently openai only), 0 otherwise. Unknown names return 0. */
int provider_supports_effort(const char *name);

#endif
