/*
 * search_tavily.c - Tavily Search provider: queries the Tavily API with
 * an API key and returns the top results as JSON. Depends on:
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
} TavilyCtx;

static char *search_tavily_search(SearchProvider *self, const char *query, int num_results)
{
    TavilyCtx *ctx = self->ctx;
    if (!ctx->api_key) return str_dup("Error: Tavily API key not configured");

    CURL *curl = curl_easy_init();
    if (!curl) return str_dup("Error: curl init failed");

    char *body = NULL;
    if (asprintf(&body, "{\"api_key\":\"%s\",\"query\":\"%s\",\"max_results\":%d}",
                 ctx->api_key, query, num_results) < 0)
    { curl_easy_cleanup(curl); return str_dup("Error: oom"); }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.tavily.com/search");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "EchoAI/1.0");

    HttpBuffer buf = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_buffer_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(body);

    if (res != CURLE_OK)
    {
        free(buf.data);
        char *err = NULL;
        if (asprintf(&err, "Error: Tavily search failed: %s",
                     curl_easy_strerror(res)) < 0) err = str_dup("Error: Tavily search failed");
        return err;
    }

    if (!buf.data) return str_dup("(no results)");

    cJSON *json = cJSON_Parse(buf.data);
    if (!json) { free(buf.data); return str_dup("Error: failed to parse Tavily response"); }

    cJSON *results = cJSON_GetObjectItem(json, "results");
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
            cJSON *content = cJSON_GetObjectItem(r, "content");
            if (title && title->valuestring)
                cJSON_AddStringToObject(item, "title", title->valuestring);
            if (url_item && url_item->valuestring)
                cJSON_AddStringToObject(item, "url", url_item->valuestring);
            if (content && content->valuestring)
                cJSON_AddStringToObject(item, "snippet", content->valuestring);
            cJSON_AddItemToArray(out_arr, item);
        }
    }

    char *result = cJSON_PrintUnformatted(out_arr);
    cJSON_Delete(out_arr);
    cJSON_Delete(json);
    free(buf.data);

    if (!result) return str_dup("(no results)");
    return result;
}

static void search_tavily_destroy(SearchProvider *self)
{
    if (!self) return;
    TavilyCtx *ctx = self->ctx;
    free(ctx->api_key);
    free(ctx);
    free(self->name);
    free(self);
}

/**
 * search_provider_tavily_create - construct the Tavily search provider
 * @api_key: Tavily API key; duplicated into the provider (owned by it)
 *
 * Return: heap-allocated SearchProvider, or NULL when api_key is NULL or
 * on OOM. Caller owns the provider and must release it via its destroy
 * function pointer.
 */
SearchProvider *search_provider_tavily_create(const char *api_key)
{
    if (!api_key) return NULL;

    SearchProvider *p = calloc(1, sizeof(SearchProvider));
    if (!p) return NULL;

    TavilyCtx *ctx = calloc(1, sizeof(TavilyCtx));
    if (!ctx) { free(p); return NULL; }

    ctx->api_key = str_dup(api_key);

    p->name = str_dup("tavily");
    p->search = search_tavily_search;
    p->destroy = search_tavily_destroy;
    p->ctx = ctx;
    return p;
}
