#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <cjson/cJSON.h>
#include <openssl/rand.h>

#include "routes.h"
#include "routes_auth.h"
#include "routes_ws.h"
#include "../middleware.h"
#include "../../agent/agent.h"
#include "../../safety/safety.h"
#include "../../session/encryption.h"
#include "../../session/session_manager.h"
#include "../../utils/logging.h"
#include "../../utils/string_utils.h"
#include "../../utils/rate_limiter.h"
#include "../../tools/registry.h"

static char *generate_unlock_token(void)
{
    unsigned char random_bytes[32];
    if (RAND_bytes(random_bytes, sizeof(random_bytes)) != 1) return NULL;

    char *token = malloc(4 + sizeof(random_bytes) * 2 + 1);
    if (!token) return NULL;
    memcpy(token, "tok_", 4);
    for (size_t i = 0; i < sizeof(random_bytes); i++)
        snprintf(token + 4 + i * 2, 3, "%02x", random_bytes[i]);
    return token;
}

void handle_setup(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    if (ctx->state != STATE_SETUP)
    {
        server_response_error(client, 400, "already configured");
        return;
    }

    if (!req->body || req->body_len == 0)
    {
        server_response_error(client, 400, "missing password");
        return;
    }

    cJSON *json = cJSON_Parse(req->body);
    if (!json)
    {
        server_response_error(client, 400, "invalid json");
        return;
    }

    cJSON *pw = cJSON_GetObjectItem(json, "password");
    if (!pw || !pw->valuestring || strlen(pw->valuestring) < 4)
    {
        cJSON_Delete(json);
        server_response_error(client, 400, "password must be at least 4 characters");
        return;
    }
    char *password = str_dup(pw->valuestring);
    cJSON_Delete(json);
    if (!password) { server_response_error(client, 500, "oom"); return; }

    const char *home = getenv("HOME");
    if (!home) { free(password); server_response_error(client, 500, "HOME not set"); return; }

    char *data_dir = NULL;
    if (asprintf(&data_dir, "%s/.config/echo-ai", home) < 0)
    { free(password); server_response_error(client, 500, "out of memory"); return; }

    SessionManager *sm = session_manager_create(data_dir, password);
    free(data_dir);

    size_t pw_len = strlen(password);
    memset(password, 0, pw_len);
    free(password);

    if (!sm)
    {
        server_response_error(client, 500, "failed to initialize session manager");
        return;
    }

    char *token = generate_unlock_token();
    if (!token)
    {
        session_manager_free(sm);
        server_response_error(client, 500, "failed to generate unlock token");
        return;
    }
    ctx->sm = sm;
    if (ctx->openai_oauth && openai_oauth_attach_session(ctx->openai_oauth, sm) != 0)
        log_error("failed to load stored OpenAI credentials", NULL);
    registry_set_session_manager(sm);
    if (ctx->agent) agent_set_session_manager(ctx->agent, sm);
    free(ctx->unlock_token);
    ctx->unlock_token = token;
    ctx->auth_generation++;
    ctx->state = STATE_UNLOCKED;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "token", ctx->unlock_token);
    cJSON_AddStringToObject(resp, "message", "echo-ai configured and unlocked");
    char *str = cJSON_PrintUnformatted(resp);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(resp);

    log_info("setup complete", NULL);
}

void handle_unlock(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    if (ctx->state != STATE_LOCKED)
    {
        server_response_error(client, 400, "not locked");
        return;
    }

    if (ctx->rate_limiter &&
        !rate_limiter_unlock_allowed(ctx->rate_limiter, req->ip, 5, 20))
    {
        server_response_error(client, 429, "too many unlock attempts, try again later");
        return;
    }

    if (!req->body || req->body_len == 0)
    {
        server_response_error(client, 400, "missing password");
        return;
    }

    cJSON *json = cJSON_Parse(req->body);
    if (!json)
    {
        server_response_error(client, 400, "invalid json");
        return;
    }

    cJSON *pw = cJSON_GetObjectItem(json, "password");
    if (!pw || !pw->valuestring)
    {
        cJSON_Delete(json);
        server_response_error(client, 400, "missing password");
        return;
    }
    char *password = str_dup(pw->valuestring);
    cJSON_Delete(json);
    if (!password) { server_response_error(client, 500, "oom"); return; }

    const char *home = getenv("HOME");
    log_debug("unlock", "home", home ? home : "NULL", NULL);
    if (!home) { free(password); server_response_error(client, 500, "HOME not set"); return; }

    char *data_dir = NULL;
    if (asprintf(&data_dir, "%s/.config/echo-ai", home) < 0)
    {
        memset(password, 0, strlen(password));
        free(password);
        server_response_error(client, 500, "oom");
        return;
    }
    SessionManagerCreateResult create_result = SESSION_MANAGER_CREATE_STORAGE_FAILED;
    SessionManager *sm = session_manager_create_ex(data_dir, password,
                                                    &create_result);
    free(data_dir);
    memset(password, 0, strlen(password));
    free(password);
    if (!sm)
    {
        if (create_result == SESSION_MANAGER_CREATE_AUTH_FAILED && ctx->rate_limiter)
            rate_limiter_record_unlock_failure(ctx->rate_limiter, req->ip);
        if (create_result == SESSION_MANAGER_CREATE_AUTH_FAILED)
            server_response_error(client, 401, "wrong password");
        else
            server_response_error(client, 500, "session storage unavailable");
        return;
    }
    char *token = generate_unlock_token();
    if (!token)
    {
        session_manager_free(sm);
        server_response_error(client, 500, "failed to generate unlock token");
        return;
    }
    ctx->sm = sm;
    if (ctx->openai_oauth && openai_oauth_attach_session(ctx->openai_oauth, sm) != 0)
        log_error("failed to load stored OpenAI credentials", NULL);
    registry_set_session_manager(sm);
    if (ctx->agent) agent_set_session_manager(ctx->agent, sm);

    free(ctx->unlock_token);
    ctx->unlock_token = token;
    ctx->auth_generation++;
    ctx->state = STATE_UNLOCKED;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "token", ctx->unlock_token);
    char *str = cJSON_PrintUnformatted(resp);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(resp);

    log_info("unlock successful", NULL);
}

void handle_logout(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    if (!middleware_check_unlock(req, ctx))
    {
        server_response_error(client, 401, "unauthorized");
        return;
    }

    if (ctx->openai_oauth && openai_oauth_attach_session(ctx->openai_oauth, NULL) != 0)
    {
        server_response_error(client, 500, "failed to lock credential storage");
        return;
    }
    free(ctx->unlock_token);
    ctx->unlock_token = NULL;
    routes_ws_invalidate_auth(ctx);
    if (ctx->agent) agent_set_session_manager(ctx->agent, NULL);
    registry_set_session_manager(NULL);
    session_manager_free(ctx->sm);
    ctx->sm = NULL;
    ctx->auth_generation++;
    ctx->state = STATE_LOCKED;
    server_response_json(client, 200, "{\"message\":\"logged out\"}");
    log_info("logout", NULL);
}

void handle_change_password(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    if (!ctx->sm)
    {
        server_response_error(client, 400, "session manager not available");
        return;
    }

    if (!req->body || req->body_len == 0)
    {
        server_response_error(client, 400, "missing body");
        return;
    }

    cJSON *json = cJSON_Parse(req->body);
    if (!json)
    {
        server_response_error(client, 400, "invalid json");
        return;
    }

    cJSON *pw = cJSON_GetObjectItem(json, "new_password");
    if (!pw || !pw->valuestring || strlen(pw->valuestring) < 4)
    {
        cJSON_Delete(json);
        server_response_error(client, 400, "new password must be at least 4 characters");
        return;
    }

    int rc = migration_change_password(ctx->sm, pw->valuestring);
    cJSON_Delete(json);

    if (rc != 0)
    {
        server_response_error(client, 500,
            rc == -2 ?
                "password changed but activation is incomplete; restart and unlock with the new password" :
                "password change failed");
        return;
    }

    server_response_json(client, 200, "{\"changed\":true}");
}
