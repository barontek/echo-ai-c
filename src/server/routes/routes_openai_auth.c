#include <stdlib.h>

#include <cjson/cJSON.h>

#include "routes_openai_auth.h"
#include "../../llm/openai_oauth.h"

static void send_status(Client *client, OpenAIOAuth *auth)
{
    char *account = NULL;
    char *plan = NULL;
    char *error = NULL;
    OpenAIOAuthState state = openai_oauth_status(auth, &account, &plan, &error);
    const char *state_name = state == OPENAI_OAUTH_PENDING ? "pending" :
        state == OPENAI_OAUTH_SIGNED_IN ? "signed_in" : "signed_out";
    cJSON *json = cJSON_CreateObject();
    if (!json)
    {
        free(account); free(plan); free(error);
        server_response_error(client, 500, "out of memory");
        return;
    }
    cJSON_AddStringToObject(json, "state", state_name);
    if (account) cJSON_AddStringToObject(json, "account_id", account);
    if (plan) cJSON_AddStringToObject(json, "plan_type", plan);
    if (error) cJSON_AddStringToObject(json, "error", error);
    char *body = cJSON_PrintUnformatted(json);
    if (body) server_response_json(client, 200, body);
    else server_response_error(client, 500, "out of memory");
    free(body); free(account); free(plan); free(error); cJSON_Delete(json);
}

void handle_openai_oauth_status(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    if (!ctx->openai_oauth) { server_response_error(client, 500, "OpenAI OAuth unavailable"); return; }
    send_status(client, ctx->openai_oauth);
}

void handle_openai_oauth_start(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    if (!ctx->openai_oauth) { server_response_error(client, 500, "OpenAI OAuth unavailable"); return; }
    char *url = NULL;
    char *login_id = NULL;
    if (openai_oauth_start(ctx->openai_oauth, &url, &login_id) != 0)
    {
        free(url); free(login_id);
        server_response_error(client, 409, "OpenAI login is already active or unavailable");
        return;
    }
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddStringToObject(json, "authorization_url", url) ||
        !cJSON_AddStringToObject(json, "login_id", login_id))
    {
        cJSON_Delete(json); free(url); free(login_id);
        server_response_error(client, 500, "out of memory");
        return;
    }
    char *body = cJSON_PrintUnformatted(json);
    if (body) server_response_json(client, 200, body);
    else server_response_error(client, 500, "out of memory");
    free(body); free(url); free(login_id); cJSON_Delete(json);
}

void handle_openai_oauth_logout(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    if (!ctx->openai_oauth || openai_oauth_logout(ctx->openai_oauth) != 0)
    {
        server_response_error(client, 500, "failed to sign out of OpenAI");
        return;
    }
    server_response_json(client, 200, "{\"signed_out\":true}");
}
