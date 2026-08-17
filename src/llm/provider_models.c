/*
 * provider_models.c - provider model-list transport: per-provider endpoint
 * resolution, curl fetch, and JSON parsing. Shared by the web server's
 * GET /api/models handler and the TUI worker's model picker so both
 * frontends resolve the same URLs and list shapes. The OpenAI path defers
 * to the OAuth catalog fetch (openai_models_fetch_alloc), which owns its
 * own transport and 401-refresh handling.
 * Depends on: libcurl, cJSON, factory.h, openai.h, openai_oauth.h.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "provider_models.h"
#include "factory.h"
#include "openai.h"
#include "openai_oauth.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

#ifdef PROVIDER_MODELS_TEST
/* Allocation-failure injection for the parse path: the test build routes
 * every str_dup through a counting shim so a mid-list OOM can be forced
 * (AGENTS.md fault-injection pattern). Production builds never define
 * PROVIDER_MODELS_TEST and see the real str_dup. The define must precede
 * parse_models_json so that function's str_dup calls are redirected. */
static int pm_test_strdup_fail_at = -1;
static int pm_test_strdup_count = 0;

static char *pm_test_strdup(const char *s)
{
    pm_test_strdup_count++;
    if (pm_test_strdup_count == pm_test_strdup_fail_at) return NULL;
    return str_dup(s);
}
#define str_dup pm_test_strdup
#endif

typedef struct {
    char *data;
    size_t len;
} ModelsBuf;

static size_t models_write_cb(void *contents, size_t size, size_t nmemb,
                              void *userdata)
{
    size_t total = size * nmemb;
    ModelsBuf *buf = userdata;
    char *tmp = realloc(buf->data, buf->len + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->len, contents, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

/* Extract every entry's name_key string from the array under list_key.
 * A shaped-but-empty or unparseable document is an empty success, matching
 * the original web handler's "nothing found" behavior. */
static int parse_models_json(const char *raw, const char *list_key,
                             const char *name_key, char ***models_out,
                             size_t *count_out)
{
    if (!raw || !list_key || !name_key || !models_out || !count_out)
        return -1;
    *models_out = NULL;
    *count_out = 0U;

    cJSON *root = cJSON_Parse(raw);
    if (!root) return 0; /* malformed body: treat as an empty catalog */

    int result = 0;
    cJSON *list = cJSON_GetObjectItem(root, list_key);
    if (list && cJSON_IsArray(list))
    {
        int count = cJSON_GetArraySize(list);
        if (count > 0)
        {
            char **models = calloc((size_t)count, sizeof(*models));
            if (!models)
            {
                result = -1;
            }
            else
            {
                int n = 0;
                for (int i = 0; i < count; i++)
                {
                    cJSON *m = cJSON_GetArrayItem(list, i);
                    cJSON *name = m ? cJSON_GetObjectItem(m, name_key) : NULL;
                    if (name && cJSON_IsString(name))
                    {
                        models[n] = str_dup(cJSON_GetStringValue(name));
                        if (!models[n])
                        {
                            for (int j = 0; j < n; j++) free(models[j]);
                            free(models);
                            result = -1;
                            break;
                        }
                        n++;
                    }
                }
                if (result == 0 && n > 0)
                {
                    *models_out = models;
                    *count_out = (size_t)n;
                }
                else if (result == 0)
                {
                    free(models); /* shaped but empty: success, no entries */
                }
            }
        }
    }
    cJSON_Delete(root);
    return result;
}

int provider_models_fetch_alloc(const char *provider, const char *base_url,
                                const char *api_token, OpenAIOAuth *oauth,
                                char ***models_out, size_t *count_out)
{
    if (!models_out || !count_out) return PROVIDER_MODELS_UNAVAILABLE;
    *models_out = NULL;
    *count_out = 0U;
    if (!provider || !provider[0]) return PROVIDER_MODELS_UNAVAILABLE;

    if (strcmp(provider, "openai") == 0)
    {
        /* OAuth-only: signed-out is an empty success, never a fallback. */
        if (!oauth || openai_oauth_status(oauth, NULL, NULL, NULL) !=
                          OPENAI_OAUTH_SIGNED_IN)
            return PROVIDER_MODELS_OK;
        int rc = openai_models_fetch_alloc(oauth, models_out, count_out);
        if (rc == OPENAI_MODELS_OK) return PROVIDER_MODELS_OK;
        if (rc == OPENAI_MODELS_DENIED) return PROVIDER_MODELS_DENIED;
        return PROVIDER_MODELS_UNAVAILABLE;
    }

    const char *path = NULL;
    const char *list_key = "models";
    const char *name_key = "name";
    if (strcmp(provider, "ollama") == 0)
    {
        path = "/api/tags";
    }
    else if (strcmp(provider, "openai_compatible") == 0)
    {
        path = "/v1/models";
        list_key = "data";
        name_key = "id";
    }
    else if (strcmp(provider, "opencode_zen") == 0 ||
             strcmp(provider, "opencode_go") == 0)
    {
        /* The Zen/Go base URLs already end in /v1. */
        path = "/models";
        list_key = "data";
        name_key = "id";
    }
    else
    {
        return PROVIDER_MODELS_UNAVAILABLE;
    }

    const char *b = base_url ? base_url : provider_default_base_url(provider);
    if (!b) return PROVIDER_MODELS_UNAVAILABLE;

    char url[1024];
    int url_len = snprintf(url, sizeof(url), "%s%s", b, path);
    if (url_len < 0 || (size_t)url_len >= sizeof(url))
        return PROVIDER_MODELS_UNAVAILABLE;

    CURL *curl = curl_easy_init();
    if (!curl) return PROVIDER_MODELS_UNAVAILABLE;

    ModelsBuf buf = {0};
    struct curl_slist *headers = NULL;
    if (api_token && api_token[0])
    {
        char *auth = NULL;
        if (asprintf(&auth, "Authorization: Bearer %s", api_token) < 0)
        {
            curl_easy_cleanup(curl);
            return PROVIDER_MODELS_UNAVAILABLE;
        }
        headers = curl_slist_append(NULL, auth);
        free(auth);
        if (!headers)
        {
            curl_easy_cleanup(curl);
            return PROVIDER_MODELS_UNAVAILABLE;
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, models_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        free(buf.data);
        return PROVIDER_MODELS_UNAVAILABLE;
    }
    int prc = parse_models_json(buf.data, list_key, name_key,
                                models_out, count_out);
    free(buf.data);
    return prc == 0 ? PROVIDER_MODELS_OK : PROVIDER_MODELS_UNAVAILABLE;
}

void provider_models_free(char **models, size_t count)
{
    if (!models) return;
    for (size_t i = 0; i < count; i++) free(models[i]);
    free(models);
}

#ifdef PROVIDER_MODELS_TEST
int provider_models_parse_test(const char *raw, const char *list_key,
                               const char *name_key, char ***models_out,
                               size_t *count_out)
{
    return parse_models_json(raw, list_key, name_key, models_out, count_out);
}

void provider_models_test_set_strdup_fail(int nth)
{
    pm_test_strdup_fail_at = nth;
    pm_test_strdup_count = 0;
}

int provider_models_test_strdup_calls(void)
{
    return pm_test_strdup_count;
}
#endif