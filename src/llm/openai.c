#include <string.h>

#include "openai.h"
#include "openai_compatible.h"

LLMProvider *openai_provider_create(const char *base_url, const char *api_token)
{
    /* Real OpenAI is the OpenAI-compatible client pointed at api.openai.com
     * with a Bearer token; the wrapper keeps the defaults in one place. */
    return openai_compatible_provider_create(
        base_url ? base_url : "https://api.openai.com", api_token);
}
