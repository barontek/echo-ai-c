#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "search_provider.h"
#include "../utils/logging.h"

SearchProvider *search_provider_brave_create(const char *api_key, const char *base_url);
SearchProvider *search_provider_duckduckgo_create(void);
SearchProvider *search_provider_tavily_create(const char *api_key);

SearchProvider *search_provider_create(const char *provider_name, const char *api_key)
{
    if (!provider_name) return NULL;

    if (strcmp(provider_name, "brave") == 0)
        return search_provider_brave_create(api_key ? api_key : "", NULL);

    if (strcmp(provider_name, "duckduckgo") == 0)
        return search_provider_duckduckgo_create();

    if (strcmp(provider_name, "tavily") == 0)
        return search_provider_tavily_create(api_key ? api_key : "");

    log_error("unknown search provider", "name", provider_name, NULL);
    return NULL;
}
