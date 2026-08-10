/*
 * search_provider.c - Factory that maps a config provider name to the
 * matching backend constructor. Depends on: search_brave, search_tavily,
 * search_duckduckgo, logging.
 */

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

    /* Backend constructors reject a NULL api_key outright, so missing
     * config becomes an empty string and surfaces at search time instead
     * of failing at create. */
    if (strcmp(provider_name, "brave") == 0)
        return search_provider_brave_create(api_key ? api_key : "", NULL);

    if (strcmp(provider_name, "duckduckgo") == 0)
        return search_provider_duckduckgo_create();

    if (strcmp(provider_name, "tavily") == 0)
        return search_provider_tavily_create(api_key ? api_key : "");

    log_error("unknown search provider", "name", provider_name, NULL);
    return NULL;
}
