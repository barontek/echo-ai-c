#include <string.h>
#include <stdlib.h>

#include "provider.h"
#include "openai.h"
#include "openai_oauth.h"
#include "openai_compatible.h"

LLMProvider *get_provider(const char *name, const char *model,
                          const char *base_url, const char *api_token,
                          int num_ctx, int keep_alive_secs,
                          const char *effort)
{
    return get_provider_with_auth(name, model, base_url, api_token,
                                  num_ctx, keep_alive_secs, effort, NULL);
}

LLMProvider *get_provider_with_auth(const char *name, const char *model,
                                    const char *base_url, const char *api_token,
                                    int num_ctx, int keep_alive_secs,
                                    const char *effort,
                                    OpenAIOAuth *openai_auth)
{
    (void)model;

    if (strcmp(name, "ollama") == 0)
    {
        extern LLMProvider *ollama_provider_create(const char *, int, int);
        return ollama_provider_create(base_url, num_ctx, keep_alive_secs);
    }

    if (strcmp(name, "openai_compatible") == 0)
    {
        return openai_compatible_provider_create(base_url, api_token);
    }

    if (strcmp(name, "lmstudio") == 0)
    {
        /* Back-compat alias: lmstudio is the OpenAI-compatible client
         * pointed at a local server. */
        return openai_compatible_provider_create(base_url, api_token);
    }

    if (strcmp(name, "openai") == 0)
    {
        if (!openai_auth) return NULL;
        return openai_provider_create(base_url, api_token, effort, openai_auth);
    }

    if (strcmp(name, "opencode_zen") == 0)
    {
        extern LLMProvider *opencode_zen_provider_create(const char *, const char *);
        return opencode_zen_provider_create(base_url, api_token);
    }

    return NULL;
}

static const char *const AVAILABLE_PROVIDERS[] = {
    "ollama",
    "openai",
    "openai_compatible",
    "opencode_zen",
};

const char *const *provider_names_available(int *count)
{
    if (count)
        *count = (int)(sizeof(AVAILABLE_PROVIDERS) / sizeof(AVAILABLE_PROVIDERS[0]));
    return AVAILABLE_PROVIDERS;
}

const char *provider_default_base_url(const char *name)
{
    if (!name) return NULL;
    if (strcmp(name, "ollama") == 0)
        return "http://localhost:11434";
    if (strcmp(name, "openai") == 0)
        return "https://chatgpt.com/backend-api/codex/responses";
    if (strcmp(name, "openai_compatible") == 0)
        return "http://localhost:1234";
    if (strcmp(name, "opencode_zen") == 0)
        return "https://opencode.ai/zen/v1";
    return NULL;
}

int provider_supports_effort(const char *name)
{
    if (!name) return 0;
    if (strcmp(name, "openai") == 0)
        return 1;
    return 0;
}
