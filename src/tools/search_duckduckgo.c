/*
 * search_duckduckgo.c - DuckDuckGo search provider: scrapes the HTML
 * results page (no API key) and returns the top results as JSON.
 * Depends on: search_provider.h, libcurl, cJSON, string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "search_provider.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} WriteBuf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    WriteBuf *buf = userdata;
    size_t total = size * nmemb;
    size_t needed = buf->len + total + 1;
    if (needed > buf->cap)
    {
        buf->cap = needed * 2;
        char *new = realloc(buf->data, buf->cap);
        if (!new) return 0;
        buf->data = new;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static char *html_extract(const char *html, const char *start_tag, const char *end_tag,
                          const char *skip)
{
    const char *p = html;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    int count = 0;

    while (p && *p && count < 8)
    {
        p = strstr(p, start_tag);
        if (!p) break;
        p += strlen(start_tag);

        if (skip && strncmp(p, skip, strlen(skip)) == 0) continue;

        const char *end = strstr(p, end_tag);
        if (!end) break;

        size_t len = (size_t)(end - p);
        if (len > 0 && len < 512)
        {
            char buf[512];
            memcpy(buf, p, len);
            buf[len] = '\0';

            /* decode common HTML entities */
            char *decoded = str_dup(buf);
            char *src = decoded;
            char *dst = decoded;
            while (*src)
            {
                if (strncmp(src, "&amp;", 5) == 0) { *dst++ = '&'; src += 5; }
                else if (strncmp(src, "&lt;", 4) == 0) { *dst++ = '<'; src += 4; }
                else if (strncmp(src, "&gt;", 4) == 0) { *dst++ = '>'; src += 4; }
                else if (strncmp(src, "&quot;", 6) == 0) { *dst++ = '"'; src += 6; }
                else if (strncmp(src, "&#39;", 5) == 0) { *dst++ = '\''; src += 5; }
                else { *dst++ = *src++; }
            }
            *dst = '\0';

            cJSON_AddItemToArray(arr, cJSON_CreateString(decoded));
            free(decoded);
            count++;
        }
        p = end;
    }

    char *result = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return result;
}

static char *search_duckduckgo_search(SearchProvider *self, const char *query, int num_results)
{
    (void)self;
    (void)num_results;

    CURL *curl = curl_easy_init();
    if (!curl) return str_dup("Error: curl init failed");

    char *url = NULL;
    char *encoded = curl_easy_escape(curl, query, 0);
    if (!encoded) { curl_easy_cleanup(curl); return str_dup("Error: oom"); }

    if (asprintf(&url, "https://html.duckduckgo.com/html/?q=%s", encoded) < 0)
    { curl_free(encoded); curl_easy_cleanup(curl); return str_dup("Error: oom"); }
    curl_free(encoded);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (EchoAI/1.0)");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    WriteBuf buf = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(url);

    if (res != CURLE_OK)
    {
        free(buf.data);
        char *err = NULL;
        if (asprintf(&err, "Error: DuckDuckGo search failed: %s",
                     curl_easy_strerror(res)) < 0) err = str_dup("Error: DuckDuckGo search failed");
        return err;
    }

    if (!buf.data) return str_dup("(no results)");

    /* Extract titles and snippets from HTML */
    char *titles = html_extract(buf.data, "class=\"result__a\">", "</a>", NULL);
    char *snippets = html_extract(buf.data, "class=\"result__snippet\">", "</a>", "class=\"result__snippet");
    char *urls = html_extract(buf.data, "class=\"result__url\"", "</a>", NULL);

    free(buf.data);

    cJSON *titles_arr = titles ? cJSON_Parse(titles) : NULL;
    cJSON *snippets_arr = snippets ? cJSON_Parse(snippets) : NULL;
    cJSON *urls_arr = urls ? cJSON_Parse(urls) : NULL;

    cJSON *out_arr = cJSON_CreateArray();
    int count = titles_arr ? cJSON_GetArraySize(titles_arr) : 0;
    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON *t = cJSON_GetArrayItem(titles_arr, i);
        cJSON *s = snippets_arr && i < cJSON_GetArraySize(snippets_arr)
                   ? cJSON_GetArrayItem(snippets_arr, i) : NULL;
        cJSON *u = urls_arr && i < cJSON_GetArraySize(urls_arr)
                   ? cJSON_GetArrayItem(urls_arr, i) : NULL;
        if (t && cJSON_IsString(t))
            cJSON_AddStringToObject(item, "title", cJSON_GetStringValue(t));
        if (s && cJSON_IsString(s))
            cJSON_AddStringToObject(item, "snippet", cJSON_GetStringValue(s));
        if (u && cJSON_IsString(u))
            cJSON_AddStringToObject(item, "url", cJSON_GetStringValue(u));
        cJSON_AddItemToArray(out_arr, item);
    }

    char *result = cJSON_PrintUnformatted(out_arr);
    cJSON_Delete(out_arr);
    if (titles_arr) cJSON_Delete(titles_arr);
    if (snippets_arr) cJSON_Delete(snippets_arr);
    if (urls_arr) cJSON_Delete(urls_arr);
    free(titles);
    free(snippets);
    free(urls);

    if (!result) return str_dup("(no results)");
    return result;
}

static void search_duckduckgo_destroy(SearchProvider *self)
{
    if (!self) return;
    free(self->ctx);
    free(self->name);
    free(self);
}

/**
 * search_provider_duckduckgo_create - construct the DuckDuckGo search
 * provider (no API key required)
 *
 * Return: heap-allocated SearchProvider, or NULL on OOM. Caller owns the
 * provider and must release it via its destroy function pointer.
 */
SearchProvider *search_provider_duckduckgo_create(void)
{
    SearchProvider *p = calloc(1, sizeof(SearchProvider));
    if (!p) return NULL;

    p->name = str_dup("duckduckgo");
    p->search = search_duckduckgo_search;
    p->destroy = search_duckduckgo_destroy;
    p->ctx = NULL;
    return p;
}
