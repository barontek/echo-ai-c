#include <string.h>
#include <stdlib.h>

#include "provider.h"

LLMProvider *get_provider(const char *name, const char *model,
                          const char *base_url, int num_ctx, int keep_alive_secs)
{
    (void)model;
    (void)num_ctx;
    (void)keep_alive_secs;

    if (strcmp(name, "ollama") == 0)
    {
        extern LLMProvider *ollama_provider_create(const char *, int, int);
        return ollama_provider_create(base_url, num_ctx, keep_alive_secs);
    }

    if (strcmp(name, "lmstudio") == 0)
    {
        /* Back-compat alias: lmstudio is the same OpenAI-compatible
         * client as openai, just pointed at a local server. */
        extern LLMProvider *openai_provider_create(const char *);
        return openai_provider_create(base_url);
    }

    if (strcmp(name, "openai") == 0)
    {
        extern LLMProvider *openai_provider_create(const char *);
        return openai_provider_create(base_url);
    }

    return NULL;
}
