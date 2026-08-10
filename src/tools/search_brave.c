/*
 * search_brave.c - Brave Search provider: queries the Brave web search
 * API with an API key and returns the top results as JSON. Depends on:
 * search_provider.h, libcurl, cJSON, string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "search_provider.h"
#include "../utils/http_client.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    char *api_key;
    char *base_url;
} BraveCtx;

/* Converts a parsed Brave API response into the shared
 * [{"title","url","snippet"}] JSON array string; caller frees. */
static char *brave_results_to_json(cJSON *json)
{
    cJSON *web = cJSON_GetObjectItem(json, "web");
    cJSON *results = web ? cJSON_GetObjectItem(web, "results") : NULL;

    cJSON *out_arr = cJSON_CreateArray();
    if (results && cJSON_IsArray(results))
    {
        int count = cJSON_GetArraySize(results);
        for (int i = 0; i < count; i++)
        {
            cJSON *r = cJSON_GetArrayItem(results, i);
            if (!r) continue;
            cJSON *item = cJSON_CreateObject();
            cJSON *title = cJSON_GetObjectItem(r, "title");
            cJSON *url_item = cJSON_GetObjectItem(r, "url");
            cJSON *desc = cJSON_GetObjectItem(r, "description");
            if (title && title->valuestring)
                cJSON_AddStringToObject(item, "title", title->valuestring);
            if (url_item && url_item->valuestring)
                cJSON_AddStringToObject(item, "url", url_item->valuestring);
            if (desc && desc->valuestring)
                cJSON_AddStringToObject(item, "snippet", desc->valuestring);
            cJSON_AddItemToArray(out_arr, item);
        }
    }

    char *result = cJSON_PrintUnformatted(out_arr);
    cJSON_Delete(out_arr);
    if (!result) return str_dup("(no results)");
    return result;
}

#ifdef SEARCH_BRAVE_TEST
/* Test-only hook: parse a raw Brave API response (no network). Caller
 * frees the returned JSON string. */
char *search_brave_test_parse_response(const char *raw)
{
    cJSON *json = cJSON_Parse(raw);
    if (!json) return str_dup("Error: failed to parse Brave response");
    char *result = brave_results_to_json(json);
    cJSON_Delete(json);
    return result;
}
#endif

static char *search_brave_search(SearchProvider *self, const char *query, int num_results)
{
    BraveCtx *ctx = self->ctx;
    if (!ctx->api_key) return str_dup("Error: Brave API key not configured");

    CURL *curl = curl_easy_init();
    if (!curl) return str_dup("Error: curl init failed");

    char *url = NULL;
    char *curl_encoded = curl_easy_escape(curl, query, 0);
    if (curl_encoded)
    {
        free(url);
        if (asprintf(&url, "%s?q=%s&count=%d&safesearch=off",
                     ctx->base_url, curl_encoded, num_results) < 0)
        { curl_free(curl_encoded); curl_easy_cleanup(curl); return str_dup("Error: oom"); }
        curl_free(curl_encoded);
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    char *auth_hdr = NULL;
    if (asprintf(&auth_hdr, "X-Subscription-Token: %s", ctx->api_key) >= 0)
    {
        headers = curl_slist_append(headers, auth_hdr);
        free(auth_hdr);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "EchoAI/1.0");

    HttpBuffer buf = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_buffer_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(url);

    if (res != CURLE_OK)
    {
        free(buf.data);
        char *err = NULL;
        if (asprintf(&err, "Error: Brave search request failed: %s",
                     curl_easy_strerror(res)) < 0) err = str_dup("Error: Brave search failed");
        return err;
    }

    if (!buf.data) return str_dup("(no results)");

    cJSON *json = cJSON_Parse(buf.data);
    if (!json) { free(buf.data); return str_dup("Error: failed to parse Brave response"); }

    char *result = brave_results_to_json(json);

    cJSON_Delete(json);
    free(buf.data);

    if (!result) return str_dup("(no results)");
    return result;
}

static void search_brave_destroy(SearchProvider *self)
{
    if (!self) return;
    BraveCtx *ctx = self->ctx;
    free(ctx->api_key);
    free(ctx->base_url);
    free(ctx);
    free(self->name);
    free(self);
}

/**
 * search_provider_brave_create - construct the Brave search provider
 * @api_key: Brave API key; duplicated into the provider (owned by it)
 * @base_url: API endpoint override, or NULL for the default
 *
 * Return: heap-allocated SearchProvider, or NULL when api_key is NULL or
 * on OOM. Caller owns the provider and must release it via its destroy
 * function pointer.
 */
SearchProvider *search_provider_brave_create(const char *api_key, const char *base_url)
{
    if (!api_key) return NULL;

    SearchProvider *p = calloc(1, sizeof(SearchProvider));
    if (!p) return NULL;

    BraveCtx *ctx = calloc(1, sizeof(BraveCtx));
    if (!ctx) { free(p); return NULL; }

    ctx->api_key = str_dup(api_key);
    ctx->base_url = str_dup(base_url ? base_url : "https://api.search.brave.com/res/v1/web/search");

    p->name = str_dup("brave");
    p->search = search_brave_search;
    p->destroy = search_brave_destroy;
    p->ctx = ctx;
    return p;
}
