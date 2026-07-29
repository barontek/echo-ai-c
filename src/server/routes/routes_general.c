#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "routes.h"
#include "routes_general.h"
#include "../middleware.h"
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

void handle_models(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    (void)ctx;
    cJSON *arr = cJSON_CreateArray();

    CURL *curl = curl_easy_init();
    if (curl)
    {
        ModelsBuf buf = {0};
        curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/tags");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, models_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK && buf.data)
        {
            cJSON *root = cJSON_Parse(buf.data);
            if (root)
            {
                cJSON *models = cJSON_GetObjectItem(root, "models");
                if (models && cJSON_IsArray(models))
                {
                    int count = cJSON_GetArraySize(models);
                    for (int i = 0; i < count; i++)
                    {
                        cJSON *m = cJSON_GetArrayItem(models, i);
                        cJSON *name = m ? cJSON_GetObjectItem(m, "name") : NULL;
                        if (name && cJSON_IsString(name))
                            cJSON_AddItemToArray(arr, cJSON_CreateString(cJSON_GetStringValue(name)));
                    }
                }
                cJSON_Delete(root);
            }
        }
        free(buf.data);
        curl_easy_cleanup(curl);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddItemToObject(resp, "models", arr);
    char *str = cJSON_PrintUnformatted(resp);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(resp);
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
