/*
 * factory.c - provider factory: name-based provider construction and
 * the static metadata tables (available names, default endpoints,
 * reasoning-effort support). Depends on: provider.h, openai, openai_oauth,
 * openai_compatible; declares ollama_provider_create and
 * opencode_zen_provider_create via extern.
 */

#include <string.h>
#include <stdlib.h>

#include "factory.h"
#include "ollama.h"
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
        return ollama_provider_create(base_url, num_ctx, keep_alive_secs, effort);
    }

    if (strcmp(name, "openai_compatible") == 0)
    {
        return openai_compatible_provider_create(base_url, api_token, effort);
    }

    if (strcmp(name, "lmstudio") == 0)
    {
        /* Back-compat alias: lmstudio is the OpenAI-compatible client
         * pointed at a local server. */
        return openai_compatible_provider_create(base_url, api_token, effort);
    }

    if (strcmp(name, "openai") == 0)
    {
        if (!openai_auth) return NULL;
        return openai_provider_create(base_url, api_token, effort, openai_auth);
    }

    if (strcmp(name, "opencode_zen") == 0)
    {
        extern LLMProvider *opencode_zen_provider_create(const char *, const char *, const char *);
        return opencode_zen_provider_create(base_url, api_token, effort);
    }

    if (strcmp(name, "opencode_go") == 0)
    {
        /* OpenCode Go gateway: OpenAI-compatible, same shape as Zen but a
         * different endpoint; keep the default here (like the zen wrapper
         * keeps its own) so a NULL base_url never falls back to the
         * openai_compatible localhost default. */
        return openai_compatible_provider_create(
            base_url ? base_url : "https://opencode.ai/zen/go/v1",
            api_token, effort);
    }

    return NULL;
}

static const char *const AVAILABLE_PROVIDERS[] = {
    "ollama",
    "openai",
    "openai_compatible",
    "opencode_zen",
    "opencode_go",
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
    if (strcmp(name, "opencode_go") == 0)
        return "https://opencode.ai/zen/go/v1";
    return NULL;
}

int provider_supports_effort(const char *name)
{
    if (!name) return 0;
    if (strcmp(name, "openai") == 0)
        return 1;
    if (strcmp(name, "openai_compatible") == 0)
        return 1;
    if (strcmp(name, "ollama") == 0)
        return 1;
    /* Zen/Go are the OpenAI-compatible client, so they take the same set. */
    if (strcmp(name, "opencode_zen") == 0)
        return 1;
    if (strcmp(name, "opencode_go") == 0)
        return 1;
    return 0;
}

/* UI display order; the wire validation mirrors these lists in the
 * providers' own *_reasoning_effort_valid functions. */
static const char *const OPENAI_EFFORT_OPTIONS[] = {
    "low", "medium", "high", "xhigh", "max", "none", NULL,
};

static const char *const OPENAI_COMPAT_EFFORT_OPTIONS[] = {
    "low", "medium", "high", "max", "none", NULL,
};

static const char *const OLLAMA_EFFORT_OPTIONS[] = {
    "low", "medium", "high", "max", "none", NULL,
};

const char *const *provider_effort_options(const char *name)
{
    if (!name) return NULL;
    if (strcmp(name, "openai") == 0)
        return OPENAI_EFFORT_OPTIONS;
    if (strcmp(name, "openai_compatible") == 0)
        return OPENAI_COMPAT_EFFORT_OPTIONS;
    if (strcmp(name, "ollama") == 0)
        return OLLAMA_EFFORT_OPTIONS;
    /* Zen/Go are the OpenAI-compatible client; they validate through
     * openai_compatible_reasoning_effort_valid, so reuse its list. */
    if (strcmp(name, "opencode_zen") == 0)
        return OPENAI_COMPAT_EFFORT_OPTIONS;
    if (strcmp(name, "opencode_go") == 0)
        return OPENAI_COMPAT_EFFORT_OPTIONS;
    return NULL;
}

int provider_effort_valid(const char *name, const char *effort)
{
    if (!effort || !effort[0]) return 1;
    const char *const *options = provider_effort_options(name);
    if (!options) return 0;
    for (int i = 0; options[i]; i++)
        if (strcmp(options[i], effort) == 0)
            return 1;
    return 0;
}
