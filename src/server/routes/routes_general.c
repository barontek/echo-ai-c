/*
 * routes_general.c - misc HTTP endpoints: status/health probes, agent
 * config, model and provider discovery, metrics rendering, and the
 * undo/redo change-tracker endpoints. Depends on: cJSON, config, provider
 * registry, provider_models, session_manager, logging, string_utils.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "routes.h"
#include "routes_general.h"
#include "../middleware.h"
#include "../../config/config.h"
#include "../../llm/provider_models.h"
#include "../../llm/factory.h"
#include "../../llm/provider.h"
#include "../../session/session_manager.h"
#include "../../utils/logging.h"
#include "../../utils/string_utils.h"

void handle_status(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    int locked = (ctx->state == STATE_LOCKED);
    int needs_setup = (ctx->state == STATE_SETUP);

    /* unlock_token "noop" marks session-management-disabled mode: the
     * server is permanently unlocked and never demands a token (mirrors
     * the WS gate's special case in server.c). */
    if (ctx->state == STATE_UNLOCKED && ctx->unlock_token &&
        strcmp(ctx->unlock_token, "noop") != 0 &&
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
    char *body = metrics_render_new(ctx->metrics);
    if (!body) {
        server_response_error(client, 500, "oom");
        return;
    }
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
                    snprintf(provider, sizeof(provider), "openai_compatible"); // NOLINT(cert-err33-c)
            }
        }
    }

    /* Unknown providers have no default endpoint: return an empty list
     * without touching the network. */
    if (!provider_default_base_url(provider))
    {
        models_response(client, arr);
        return;
    }

    /* Resolve the per-provider base URL the way startup does ([<provider>.
     * base_url] override, else the provider's canonical default); the
     * shared fetcher applies the default when this stays NULL. OpenAI is
     * OAuth-only: no static token is ever sent on its behalf. */
    char provider_key[96];
    snprintf(provider_key, sizeof(provider_key), "%s.base_url", provider); // NOLINT(cert-err33-c)
    const char *base_url = ctx && ctx->conf
        ? conf_get(ctx->conf, provider_key) : NULL;
    const char *api_token = ctx && ctx->conf &&
                            strcmp(provider, "openai") != 0
        ? conf_provider_token(ctx->conf, provider) : NULL;

    char **models = NULL;
    size_t count = 0U;
    int rc = provider_models_fetch_alloc(provider, base_url, api_token,
                                         ctx ? ctx->openai_oauth : NULL,
                                         &models, &count);
    if (rc == PROVIDER_MODELS_OK)
    {
        for (size_t i = 0; i < count; i++)
        {
            cJSON *name = cJSON_CreateString(models[i]);
            if (!name || !cJSON_AddItemToArray(arr, name))
            {
                cJSON_Delete(name);
                provider_models_free(models, count);
                cJSON_Delete(arr);
                server_response_error(client, 500, "oom");
                return;
            }
        }
        provider_models_free(models, count);
        models_response(client, arr);
        return;
    }
    provider_models_free(models, count);

    if (rc == PROVIDER_MODELS_DENIED)
    {
        /* 4xx entitlement denial: an empty list beats offering models the
         * account cannot use. */
        models_response(client, arr);
        return;
    }

    /* Transport/discovery failure: only OpenAI falls back to a fixed
     * catalog; local endpoints return an empty list. */
    if (strcmp(provider, "openai") == 0)
    {
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
    }
    models_response(client, arr);
}

void handle_providers(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    (void)ctx;
    cJSON *json = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON *effort_arr = cJSON_CreateArray();
    cJSON *effort_opts = cJSON_CreateObject();
    if (json && arr)
    {
        int count = 0;
        const char *const *names = provider_names_available(&count);
        for (int i = 0; i < count; i++)
        {
            cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
            /* Tells the UI which providers accept a reasoning-effort hint so
             * it can gate the effort selector per provider. */
            if (provider_supports_effort(names[i]))
            {
                cJSON_AddItemToArray(effort_arr, cJSON_CreateString(names[i]));
                /* Per-provider option lists; the UI renders exactly these
                 * (openai has xhigh, openai_compatible does not). */
                const char *const *options = provider_effort_options(names[i]);
                cJSON *opt_arr = cJSON_CreateArray();
                if (opt_arr && options)
                {
                    for (int j = 0; options[j]; j++)
                        cJSON_AddItemToArray(opt_arr, cJSON_CreateString(options[j]));
                }
                cJSON_AddItemToObject(effort_opts, names[i], opt_arr);
            }
        }
        cJSON_AddItemToObject(json, "providers", arr);
    }
    if (json && effort_arr)
        cJSON_AddItemToObject(json, "effort_supported", effort_arr);
    else
        cJSON_Delete(effort_arr);
    if (json && effort_opts && cJSON_GetArraySize(effort_opts) > 0)
        cJSON_AddItemToObject(json, "effort_options", effort_opts);
    else
        cJSON_Delete(effort_opts);
    char *str = json ? cJSON_PrintUnformatted(json) : NULL;
    server_response_json(client, 200, str ? str : "{\"providers\":[]}");
    free(str);
    cJSON_Delete(json);
}

