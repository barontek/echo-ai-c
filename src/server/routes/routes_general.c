#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "routes.h"
#include "routes_general.h"
#include "../middleware.h"
#include "../../config/config.h"
#include "../../llm/openai.h"
#include "../../llm/provider.h"
#include "../../llm/openai_oauth.h"
#include "../../session/session_manager.h"
#include "../../utils/logging.h"
#include "../../utils/string_utils.h"

void handle_status(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    int locked = (ctx->state == STATE_LOCKED);
    int needs_setup = (ctx->state == STATE_SETUP);

    if (ctx->state == STATE_UNLOCKED && ctx->unlock_token &&
        !middleware_has_valid_token(req->headers, ctx->unlock_token))
        locked = 1;

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "locked", locked);
    cJSON_AddBoolToObject(json, "needs_setup", needs_setup);
    cJSON_AddBoolToObject(json, "session_enabled", ctx->sm != NULL);
    char *str = cJSON_PrintUnformatted(json);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(json);
}

void handle_health(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    (void)ctx;
    server_response_json(client, 200, "{\"status\":\"ok\"}");
}

void handle_config(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    cJSON *cfg = cJSON_CreateObject();
    cJSON *inner = cJSON_CreateObject();
    if (ctx->agent)
    {
        cJSON_AddStringToObject(inner, "provider", ctx->agent->model ? "ollama" : "");
        cJSON_AddStringToObject(inner, "model", ctx->agent->model ? ctx->agent->model : "");
        cJSON_AddNumberToObject(inner, "temperature", ctx->agent->temperature);
        cJSON_AddNumberToObject(inner, "max_iterations", ctx->agent->max_iterations);
    }
    else
    {
        cJSON_AddStringToObject(inner, "provider", "ollama");
        cJSON_AddStringToObject(inner, "model", "");
        cJSON_AddNumberToObject(inner, "temperature", 0.7);
        cJSON_AddNumberToObject(inner, "max_iterations", 50);
    }
    cJSON_AddBoolToObject(inner, "session_enabled", ctx->sm != NULL);
    cJSON_AddItemToObject(cfg, "config", inner);
    char *str = cJSON_PrintUnformatted(cfg);
    if (str)
    {
        server_response_json(client, 200, str);
        free(str);
    }
    else
        server_response_error(client, 500, "oom");
    cJSON_Delete(cfg);
}

void handle_metrics(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    if (!ctx->metrics)
    {
        server_response_error(client, 500, "metrics not available");
        return;
    }
    char *body = metrics_render(ctx->metrics);
    if (!body) { server_response_error(client, 500, "oom"); return; }
    server_response(client, 200, "text/plain; charset=utf-8", body);
    free(body);
}

void handle_undo(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    if (!ctx->change_tracker)
    {
        server_response_error(client, 400, "change tracker not available");
        return;
    }
    int rc = ct_undo(ctx->change_tracker);
    if (rc < 0)
    {
        server_response_json(client, 200, "{\"undo\":false,\"reason\":\"nothing to undo\"}");
        return;
    }
    char *resp = NULL;
    if (asprintf(&resp, "{\"undo\":true,\"bytes_restored\":%d}", rc) < 0)
        resp = NULL;
    server_response_json(client, 200, resp ? resp : "{\"undo\":true}");
    free(resp);
}

void handle_redo(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    if (!ctx->change_tracker)
    {
        server_response_error(client, 400, "change tracker not available");
        return;
    }
    int rc = ct_redo(ctx->change_tracker);
    if (rc < 0)
    {
        server_response_json(client, 200, "{\"redo\":false,\"reason\":\"nothing to redo\"}");
        return;
    }
    char *resp = NULL;
    if (asprintf(&resp, "{\"redo\":true,\"bytes_written\":%d}", rc) < 0)
        resp = NULL;
    server_response_json(client, 200, resp ? resp : "{\"redo\":true}");
    free(resp);
}

void handle_health_detailed(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "session_enabled", ctx->sm != NULL);

    if (ctx->sm)
    {
        SessionList *list = session_manager_list_sessions(ctx->sm);
        cJSON_AddNumberToObject(json, "session_count", list ? list->count : 0);
        if (list) session_list_free(list);
    }

    cJSON_AddStringToObject(json, "state",
        ctx->state == STATE_UNLOCKED ? "unlocked" :
        ctx->state == STATE_SETUP ? "setup" : "locked");

    char *str = cJSON_PrintUnformatted(json);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(json);
}

typedef struct {
    char *data;
    size_t len;
} ModelsBuf;

static size_t models_write_cb(void *contents, size_t size, size_t nmemb, void *userdata)
{
    size_t total = size * nmemb;
    ModelsBuf *buf = (ModelsBuf *)userdata;
    char *tmp = realloc(buf->data, buf->len + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->len, contents, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

/* Emit {"models":[...]}; takes ownership of arr. */
static void models_response(Client *client, cJSON *arr)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddItemToObject(resp, "models", arr);
    char *str = cJSON_PrintUnformatted(resp);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(resp);
}

void handle_models(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    cJSON *arr = cJSON_CreateArray();

    /* Provider from the query string. The FE sends it on every model
     * refetch; when absent we default to ollama (the FE also calls
     * this without a provider on first load). */
    char provider[64] = "ollama";
    const char *q = req ? req->query : NULL;
    if (q)
    {
        const char *p = strstr(q, "provider=");
        if (p)
        {
            p += 9;
            size_t plen = 0;
            while (p[plen] != '\0' && p[plen] != '&' && plen < sizeof(provider) - 1)
                plen++;
            if (plen > 0)
            {
                memcpy(provider, p, plen);
                provider[plen] = '\0';
                /* FE spelling is lm_studio; use the static-token compatible provider. */
                if (strcmp(provider, "lm_studio") == 0)
                    snprintf(provider, sizeof(provider), "openai_compatible");
            }
        }
    }

    const char *base_url = NULL;
    const char *default_base = NULL;
    const char *path = NULL;
    const char *list_key = "models";
    const char *name_key = "name";
    if (strcmp(provider, "openai") == 0)
    {
        if (!ctx || !ctx->openai_oauth ||
            openai_oauth_status(ctx->openai_oauth, NULL, NULL, NULL) != OPENAI_OAUTH_SIGNED_IN)
        {
            models_response(client, arr);
            return;
        }
        char **remote_models = NULL;
        size_t remote_count = 0U;
        int fetch_rc = openai_models_fetch_alloc(ctx->openai_oauth,
                                                 &remote_models, &remote_count);
        if (fetch_rc == OPENAI_MODELS_OK && remote_count > 0U)
        {
            for (size_t i = 0; i < remote_count; i++)
            {
                cJSON *name = cJSON_CreateString(remote_models[i]);
                if (!name || !cJSON_AddItemToArray(arr, name))
                {
                    cJSON_Delete(name);
                    openai_models_free(remote_models, remote_count);
                    cJSON_Delete(arr);
                    server_response_error(client, 500, "oom");
                    return;
                }
            }
            openai_models_free(remote_models, remote_count);
            models_response(client, arr);
            return;
        }
        openai_models_free(remote_models, remote_count);
        /* Signed in with nothing listable, or the backend denied the account:
         * an empty list beats offering models the account cannot use. */
        if (fetch_rc == OPENAI_MODELS_OK || fetch_rc == OPENAI_MODELS_DENIED)
        {
            models_response(client, arr);
            return;
        }
        log_warn("OpenAI model discovery failed; using fallback catalog", NULL);
        static const char *const fallback_models[] = {
            "gpt-5.5", "gpt-5.4", "gpt-5.4-mini", "gpt-5.3-codex-spark"
        };
        for (size_t i = 0; i < sizeof(fallback_models) / sizeof(fallback_models[0]); i++)
        {
            cJSON *name = cJSON_CreateString(fallback_models[i]);
            if (!name || !cJSON_AddItemToArray(arr, name))
            {
                cJSON_Delete(name);
                cJSON_Delete(arr);
                server_response_error(client, 500, "oom");
                return;
            }
        }
        models_response(client, arr);
        return;
    }
    if (strcmp(provider, "openai_compatible") == 0)
    {
        /* OpenAI-compatible endpoint (LM Studio, vLLM, ...). */
        base_url = ctx && ctx->conf
            ? conf_get(ctx->conf, "openai_compatible.base_url") : NULL;
        default_base = "http://localhost:1234";
        path = "/v1/models";
        list_key = "data";
        name_key = "id";
    }
    else if (strcmp(provider, "opencode_zen") == 0)
    {
        /* OpenCode Zen: OpenAI-compatible endpoint at opencode.ai/zen/v1. */
        base_url = ctx && ctx->conf
            ? conf_get(ctx->conf, "opencode_zen.base_url") : NULL;
        default_base = "https://opencode.ai/zen/v1";
        path = "/models";  /* base_url already includes /v1 */
        list_key = "data";
        name_key = "id";
    }
    else if (strcmp(provider, "ollama") == 0)
    {
        base_url = ctx && ctx->conf ? conf_get(ctx->conf, "ollama.base_url") : NULL;
        default_base = "http://localhost:11434";
        path = "/api/tags";
    }
    else
    {
        /* Unsupported provider (anthropic): nothing to list. */
        models_response(client, arr);
        return;
    }

    char url[1024];
    int url_len = snprintf(url, sizeof(url), "%s%s",
                           base_url ? base_url : default_base, path);
    if (url_len < 0 || (size_t)url_len >= sizeof(url))
    {
        /* Truncated URL can't be queried; return an empty list. */
        models_response(client, arr);
        return;
    }

    CURL *curl = curl_easy_init();
    if (curl)
    {
        ModelsBuf buf = {0};
        struct curl_slist *headers = NULL;
        const char *api_token = ctx && ctx->conf
            ? conf_provider_token(ctx->conf, provider) : NULL;
        if (api_token && api_token[0])
        {
            char *auth = NULL;
            if (asprintf(&auth, "Authorization: Bearer %s", api_token) < 0)
            {
                curl_easy_cleanup(curl);
                models_response(client, arr);
                return;
            }
            headers = curl_slist_append(NULL, auth);
            free(auth);
            if (!headers)
            {
                curl_easy_cleanup(curl);
                models_response(client, arr);
                return;
            }
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, models_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK && buf.data)
        {
            cJSON *root = cJSON_Parse(buf.data);
            if (root)
            {
                cJSON *list = cJSON_GetObjectItem(root, list_key);
                if (list && cJSON_IsArray(list))
                {
                    int count = cJSON_GetArraySize(list);
                    for (int i = 0; i < count; i++)
                    {
                        cJSON *m = cJSON_GetArrayItem(list, i);
                        cJSON *name = m ? cJSON_GetObjectItem(m, name_key) : NULL;
                        if (name && cJSON_IsString(name))
                            cJSON_AddItemToArray(arr, cJSON_CreateString(cJSON_GetStringValue(name)));
                    }
                }
                cJSON_Delete(root);
            }
        }
        free(buf.data);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    models_response(client, arr);
}

void handle_providers(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    (void)ctx;
    cJSON *json = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (json && arr)
    {
        int count = 0;
        const char *const *names = provider_names_available(&count);
        for (int i = 0; i < count; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
        cJSON_AddItemToObject(json, "providers", arr);
    }
    char *str = json ? cJSON_PrintUnformatted(json) : NULL;
    server_response_json(client, 200, str ? str : "{\"providers\":[]}");
    free(str);
    cJSON_Delete(json);
}

void handle_preferences_get(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    (void)ctx;
    cJSON *resp = cJSON_CreateObject();
    cJSON *prefs = cJSON_CreateObject();
    cJSON_AddItemToObject(resp, "preferences", prefs);
    char *str = cJSON_PrintUnformatted(resp);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(resp);
}

void handle_preferences_set(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    (void)ctx;
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "saved", 1);
    char *str = cJSON_PrintUnformatted(resp);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(resp);
}
