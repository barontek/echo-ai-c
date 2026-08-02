#include <string.h>

#include "opencode_zen.h"
#include "openai_compatible.h"

LLMProvider *opencode_zen_provider_create(const char *base_url,
                                          const char *api_token)
{
    /* OpenCode Zen is the OpenAI-compatible client pointed at
     * opencode.ai/zen/v1 with a Bearer token; the wrapper keeps the
     * defaults in one place. */
    return openai_compatible_provider_create(
        base_url ? base_url : "https://opencode.ai/zen/v1", api_token);
}
