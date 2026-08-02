#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "routes_openai_auth.h"
#include "../../llm/openai_oauth.h"

static const char *state_name(OpenAIOAuthState state)
{
    return state == OPENAI_OAUTH_PENDING ? "pending" :
        state == OPENAI_OAUTH_SIGNED_IN ? "signed_in" : "signed_out";
}

static void send_status(Client *client, OpenAIOAuthState state,
                        char *account, char *plan, char *error)
{
    cJSON *json = cJSON_CreateObject();
    if (!json)
    {
        free(account); free(plan); free(error);
        server_response_error(client, 500, "out of memory");
        return;
    }
    int ok = cJSON_AddStringToObject(json, "state", state_name(state)) != NULL;
    if (account) ok = ok && cJSON_AddStringToObject(json, "account_id", account) != NULL;
    if (plan) ok = ok && cJSON_AddStringToObject(json, "plan_type", plan) != NULL;
    if (error) ok = ok && cJSON_AddStringToObject(json, "error", error) != NULL;
    if (!ok)
    {
        free(account); free(plan); free(error); cJSON_Delete(json);
        server_response_error(client, 500, "out of memory");
        return;
    }
    char *body = cJSON_PrintUnformatted(json);
    if (body) server_response_json(client, 200, body);
    else server_response_error(client, 500, "out of memory");
    free(body); free(account); free(plan); free(error); cJSON_Delete(json);
}

static int login_id_from_query(const HTTPRequest *req, char **login_id)
{
    *login_id = NULL;
    if (!req || !req->query[0]) return 0;
    const char prefix[] = "login_id=";
    if (strncmp(req->query, prefix, sizeof(prefix) - 1) != 0) return -1;
    const char *value = req->query + sizeof(prefix) - 1;
    size_t length = strlen(value);
    if (length == 0 || length > 128 || strchr(value, '&')) return -1;
    for (size_t i = 0; i < length; i++)
    {
        unsigned char c = (unsigned char)value[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return -1;
    }
    *login_id = malloc(length + 1);
    if (!*login_id) return -1;
    memcpy(*login_id, value, length + 1);
    return 0;
}

void handle_openai_oauth_status(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    if (!ctx->openai_oauth) { server_response_error(client, 500, "OpenAI OAuth unavailable"); return; }
    char *login_id = NULL;
    if (login_id_from_query(req, &login_id) != 0)
    { server_response_error(client, 400, "invalid OpenAI login id"); return; }
    char *account = NULL;
    char *plan = NULL;
    char *error = NULL;
    OpenAIOAuthState state = OPENAI_OAUTH_SIGNED_OUT;
    int result = login_id ? openai_oauth_status_for_login(
        ctx->openai_oauth, login_id, &state, &account, &plan, &error) : 0;
    if (!login_id)
        state = openai_oauth_status(ctx->openai_oauth, &account, &plan, &error);
    free(login_id);
    if (result != 0)
    {
        free(account); free(plan); free(error);
        server_response_error(client, 404, "OpenAI login is not active");
        return;
    }
    send_status(client, state, account, plan, error);
}

void handle_openai_oauth_start(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    if (!ctx->openai_oauth || !ctx->sm)
    { server_response_error(client, 503, "OpenAI OAuth requires unlocked credential storage"); return; }
    char *url = NULL;
    char *login_id = NULL;
    if (openai_oauth_start(ctx->openai_oauth, &url, &login_id) != 0)
    {
        free(url); free(login_id);
        int status = openai_oauth_status(ctx->openai_oauth, NULL, NULL, NULL) ==
                     OPENAI_OAUTH_PENDING ? 409 : 500;
        server_response_error(client, status, status == 409 ?
                              "OpenAI login is already active" :
                              "OpenAI login could not be started");
        return;
    }
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddStringToObject(json, "authorization_url", url) ||
        !cJSON_AddStringToObject(json, "login_id", login_id))
    {
        (void)openai_oauth_cancel_login(ctx->openai_oauth, login_id);
        cJSON_Delete(json); free(url); free(login_id);
        server_response_error(client, 500, "out of memory");
        return;
    }
    char *body = cJSON_PrintUnformatted(json);
    if (body) server_response_json(client, 200, body);
    else
    {
        (void)openai_oauth_cancel_login(ctx->openai_oauth, login_id);
        server_response_error(client, 500, "out of memory");
    }
    free(body); free(url); free(login_id); cJSON_Delete(json);
}

void handle_openai_oauth_logout(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    if (!ctx->openai_oauth)
    { server_response_error(client, 500, "OpenAI OAuth unavailable"); return; }
    char *login_id = NULL;
    if (req && req->body && req->body_len > 0)
    {
        cJSON *json = cJSON_ParseWithLength(req->body, req->body_len);
        cJSON *id = json ? cJSON_GetObjectItemCaseSensitive(json, "login_id") : NULL;
        if (!json || !id || !cJSON_IsString(id) || !id->valuestring || !id->valuestring[0])
        {
            cJSON_Delete(json);
            server_response_error(client, 400, "invalid OpenAI login id");
            return;
        }
        size_t login_id_len = strlen(id->valuestring);
        login_id = malloc(login_id_len + 1);
        if (login_id) memcpy(login_id, id->valuestring, login_id_len + 1);
        cJSON_Delete(json);
        if (!login_id)
        { server_response_error(client, 500, "out of memory"); return; }
    }
    int result = login_id ? openai_oauth_cancel_login(ctx->openai_oauth, login_id) :
                            openai_oauth_logout(ctx->openai_oauth);
    free(login_id);
    if (result != 0)
    {
        server_response_error(client, 500, "failed to cancel or sign out of OpenAI");
        return;
    }
    server_response_json(client, 200, "{\"signed_out\":true}");
}
