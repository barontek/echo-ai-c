#include "llm/provider.h"

/* Agent unit tests provide their own get_provider mock. Keep the new OAuth
 * factory entry point delegated to that mock without linking the production
 * provider graph into every agent test. */
LLMProvider *get_provider_with_auth(const char *name, const char *model,
                                    const char *base_url, const char *api_token,
                                    int num_ctx, int keep_alive_secs,
                                    OpenAIOAuth *openai_auth)
{
    (void)openai_auth;
    return get_provider(name, model, base_url, api_token, num_ctx, keep_alive_secs);
}
