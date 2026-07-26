#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "routes.h"
#include "websocket.h"
#include "middleware.h"
#include "../agent/agent.h"
#include "../safety/safety.h"
#include "../session/encryption.h"
#include "../session/session_manager.h"
#include "../session/memory.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"
#include "../utils/rate_limiter.h"
#include "../tools/registry.h"

static void handle_status(HTTPRequest *req, Client *client, ServerContext *ctx)
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

static void handle_health(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    (void)ctx;
    server_response_json(client, 200, "{\"status\":\"ok\"}");
}

static void handle_config(HTTPRequest *req, Client *client, ServerContext *ctx)
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

static void handle_setup(HTTPRequest *req, Client *client, ServerContext *ctx)
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

    char *salt_path = NULL;
    if (asprintf(&salt_path, "%s/.config/echo-ai/salt", home) < 0)
    { free(password); server_response_error(client, 500, "out of memory"); return; }

    if (encryption_salt_create(salt_path) != 0)
    {
        free(salt_path);
        free(password);
        server_response_error(client, 500, "failed to create salt");
        return;
    }
    free(salt_path);

    char *pw_path = NULL;
    if (asprintf(&pw_path, "%s/.config/echo-ai/password", home) >= 0)
    {
        FILE *f = fopen(pw_path, "w");
        if (f)
        {
            fputs(password, f);
            fclose(f);
        }
        free(pw_path);
    }

    char token[64];
    snprintf(token, sizeof(token), "tok_%ld_%d", (long)time(NULL), rand() % 100000);
    ctx->unlock_token = str_dup(token);
    ctx->state = STATE_UNLOCKED;
    free(password);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "token", token);
    cJSON_AddStringToObject(resp, "message", "echo-ai configured and unlocked");
    char *str = cJSON_PrintUnformatted(resp);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(resp);

    log_info("setup complete", NULL);
}

static void handle_unlock(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    if (ctx->state != STATE_LOCKED && ctx->state != STATE_UNLOCKED)
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

    char *salt_path = NULL;
    if (asprintf(&salt_path, "%s/.config/echo-ai/salt", home) < 0)
    { free(password); server_response_error(client, 500, "oom"); return; }

    unsigned char salt[64];
    int salt_len = 0;
    if (encryption_salt_load(salt_path, salt, &salt_len) != 0)
    {
        free(salt_path);
        free(password);
        server_response_error(client, 500, "salt not found");
        return;
    }

    EncryptionKey key;
    if (encryption_key_derive(password, salt, salt_len, &key) != 0)
    {
        free(salt_path);
        free(password);
        server_response_error(client, 500, "key derivation failed");
        return;
    }

    char *verifier_path = NULL;
    if (asprintf(&verifier_path, "%s/.config/echo-ai/.verifier", home) < 0)
    { memset(&key, 0, sizeof(key)); free(salt_path); free(password); server_response_error(client, 500, "oom"); return; }

    int rc = encryption_check_verifier(&key, verifier_path);
    log_debug("verifier", "result", rc == 0 ? "ok" : "fail", NULL);
    free(verifier_path);
    if (rc != 0)
    {
        memset(&key, 0, sizeof(key));
        if (ctx->rate_limiter)
            rate_limiter_record_unlock_failure(ctx->rate_limiter, req->ip);
        free(salt_path);
        free(password);
        server_response_error(client, 401, "wrong password");
        return;
    }

    memset(&key, 0, sizeof(key));
    free(salt_path);

    char *pw_path = NULL;
    if (asprintf(&pw_path, "%s/.config/echo-ai/password", home) >= 0)
    {
        FILE *f = fopen(pw_path, "w");
        if (f)
        {
            fputs(password, f);
            fclose(f);
        }
        free(pw_path);
    }

    if (!ctx->sm)
    {
        char *data_dir = NULL;
        if (asprintf(&data_dir, "%s/.config/echo-ai", home) >= 0)
        {
            SessionManager *sm = session_manager_create(data_dir, password);
            free(data_dir);
            if (sm)
            {
                ctx->sm = sm;
                registry_set_session_manager(sm);
                memory_table_init(sm->db);
            }
        }
    }

    free(password);

    char token[64];
    snprintf(token, sizeof(token), "tok_%ld_%d", (long)time(NULL), rand() % 100000);
    ctx->unlock_token = str_dup(token);
    ctx->state = STATE_UNLOCKED;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "token", token);
    char *str = cJSON_PrintUnformatted(resp);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(resp);

    log_info("unlock successful", NULL);
}

static void handle_logout(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    if (!middleware_check_unlock(req, ctx))
    {
        server_response_error(client, 401, "unauthorized");
        return;
    }

    free(ctx->unlock_token);
    ctx->unlock_token = NULL;
    ctx->state = STATE_LOCKED;
    server_response_json(client, 200, "{\"message\":\"logged out\"}");
    log_info("logout", NULL);
}

static void handle_sessions(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    if (!middleware_check_unlock(req, ctx))
    {
        server_response_error(client, 401, "unauthorized");
        return;
    }

    if (!ctx->sm)
    {
        server_response_json(client, 200, "{\"sessions\":[],\"session_enabled\":false}");
        return;
    }

    SessionList *list = session_manager_list_sessions(ctx->sm);
    if (!list)
    {
        server_response_json(client, 200, "{\"sessions\":[]}");
        return;
    }

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < list->count; i++)
    {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "id", list->ids[i]);
        cJSON_AddStringToObject(s, "session_id", list->ids[i]);
        cJSON_AddStringToObject(s, "title", list->titles[i]);
        cJSON_AddStringToObject(s, "created_at", list->created_ats[i]);
        cJSON_AddItemToArray(arr, s);
    }
    session_list_free(list);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddItemToObject(resp, "sessions", arr);
    char *str = cJSON_PrintUnformatted(resp);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(resp);
}

static void handle_create_session(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    if (!middleware_check_unlock(req, ctx))
    {
        server_response_error(client, 401, "unauthorized");
        return;
    }

    if (!ctx->sm)
    {
        server_response_error(client, 400, "session manager not available");
        return;
    }

    char *title = str_dup("Chat Session");
    if (req->body && req->body_len > 0)
    {
        cJSON *json = cJSON_Parse(req->body);
        if (json)
        {
            cJSON *t = cJSON_GetObjectItem(json, "title");
            if (t && t->valuestring) {
                free(title);
                title = str_dup(t->valuestring);
            }
            cJSON_Delete(json);
        }
    }

    Session *s = session_manager_create_session(ctx->sm, title);
    free(title);
    if (!s)
    {
        server_response_error(client, 500, "failed to create session");
        return;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "session_id", s->id);
    cJSON_AddStringToObject(resp, "title", s->title);
    cJSON_AddStringToObject(resp, "created_at", s->created_at);
    char *str = cJSON_PrintUnformatted(resp);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(resp);
    session_free(s);
}

static const char *session_id_from_path(const char *path)
{
    const char *prefix = "/api/sessions/";
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) == 0 && strlen(path) > plen)
        return path + plen;
    return NULL;
}

static int is_export_path(const char *sid)
{
    size_t slen = strlen(sid);
    return slen > 7 && strcmp(sid + slen - 7, "/export") == 0;
}

static void ws_add_message_to_json(cJSON *m, const Message *msg)
{
    cJSON_AddStringToObject(m, "role", msg->role ? msg->role : "unknown");
    cJSON_AddStringToObject(m, "content", msg->content ? msg->content : "");

    if (msg->thinking)
        cJSON_AddStringToObject(m, "thinking", msg->thinking);

    if (msg->tool_name)
        cJSON_AddStringToObject(m, "tool_name", msg->tool_name);

    if (msg->tool_call_id)
        cJSON_AddStringToObject(m, "tool_call_id", msg->tool_call_id);

    if (msg->error_category)
        cJSON_AddStringToObject(m, "error_category", msg->error_category);

    if (msg->tool_calls && msg->tool_calls_count > 0)
    {
        cJSON *tc_arr = cJSON_CreateArray();
        for (int j = 0; j < msg->tool_calls_count; j++)
        {
            cJSON *tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "name", msg->tool_calls[j].name ? msg->tool_calls[j].name : "");
            cJSON_AddStringToObject(tc, "arguments",
                msg->tool_calls[j].arguments ? msg->tool_calls[j].arguments : "{}");
            cJSON_AddItemToArray(tc_arr, tc);
        }
        cJSON_AddItemToObject(m, "tool_calls", tc_arr);
        cJSON_AddBoolToObject(m, "has_tools", 1);
    }
}

static void handle_session_get(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    if (!ctx->sm)
    {
        server_response_error(client, 400, "session manager not available");
        return;
    }

    const char *sid = session_id_from_path(req->path);
    if (!sid)
    {
        server_response_error(client, 400, "missing session id");
        return;
    }

    if (is_export_path(sid))
    {
        char *id_copy = str_dup(sid);
        if (!id_copy) { server_response_error(client, 500, "oom"); return; }
        id_copy[strlen(id_copy) - 7] = '\0';
        char *exported = session_manager_export_session(ctx->sm, id_copy);
        free(id_copy);
        if (exported)
        {
            server_response_json(client, 200, exported);
            free(exported);
        }
        else
            server_response_error(client, 404, "session not found");
        return;
    }

    Session *s = session_manager_load_session(ctx->sm, sid);
    if (!s)
    {
        server_response_error(client, 404, "session not found");
        return;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "id", s->id);
    cJSON_AddStringToObject(resp, "session_id", s->id);
    cJSON_AddStringToObject(resp, "title", s->title);
    cJSON_AddStringToObject(resp, "created_at", s->created_at);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s->messages_count; i++)
    {
        cJSON *m = cJSON_CreateObject();
        ws_add_message_to_json(m, &s->messages[i]);
        cJSON_AddItemToArray(arr, m);
    }
    cJSON_AddItemToObject(resp, "messages", arr);
    char *str = cJSON_PrintUnformatted(resp);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(resp);
    session_free(s);
}

static void handle_session_delete(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    if (!ctx->sm)
    {
        server_response_error(client, 400, "session manager not available");
        return;
    }

    const char *sid = session_id_from_path(req->path);
    if (!sid)
    {
        server_response_error(client, 400, "missing session id");
        return;
    }

    if (session_manager_delete_session(ctx->sm, sid) != 0)
    {
        server_response_error(client, 404, "session not found");
        return;
    }

    server_response_json(client, 200, "{\"deleted\":true}");
}

static void handle_session_update(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    if (!ctx->sm)
    {
        server_response_error(client, 400, "session manager not available");
        return;
    }

    const char *sid = session_id_from_path(req->path);
    if (!sid)
    {
        server_response_error(client, 400, "missing session id");
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

    cJSON *title = cJSON_GetObjectItem(json, "title");
    if (!title || !title->valuestring)
    {
        cJSON_Delete(json);
        server_response_error(client, 400, "missing title");
        return;
    }

    Session *s = session_manager_load_session(ctx->sm, sid);
    if (!s)
    {
        cJSON_Delete(json);
        server_response_error(client, 404, "session not found");
        return;
    }

    free(s->title);
    s->title = str_dup(title->valuestring);
    int rc = session_manager_save_session(ctx->sm, s);
    cJSON_Delete(json);

    if (rc != 0)
    {
        session_free(s);
        server_response_error(client, 500, "failed to save session");
        return;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "id", s->id);
    cJSON_AddStringToObject(resp, "session_id", s->id);
    cJSON_AddStringToObject(resp, "title", s->title);
    cJSON_AddStringToObject(resp, "created_at", s->created_at);
    char *str = cJSON_PrintUnformatted(resp);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(resp);
    session_free(s);
}

static void handle_sessions_rename(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    if (!middleware_check_unlock(req, ctx))
    {
        server_response_error(client, 401, "unauthorized");
        return;
    }

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

    cJSON *sid_item = cJSON_GetObjectItem(json, "session_id");
    cJSON *title_item = cJSON_GetObjectItem(json, "new_title");
    if (!sid_item || !sid_item->valuestring || !title_item || !title_item->valuestring)
    {
        cJSON_Delete(json);
        server_response_error(client, 400, "missing session_id or new_title");
        return;
    }

    Session *s = session_manager_load_session(ctx->sm, sid_item->valuestring);
    if (!s)
    {
        cJSON_Delete(json);
        server_response_error(client, 404, "session not found");
        return;
    }

    free(s->title);
    s->title = str_dup(title_item->valuestring);
    int rc = session_manager_save_session(ctx->sm, s);
    cJSON_Delete(json);

    if (rc != 0)
    {
        session_free(s);
        server_response_error(client, 500, "failed to save session");
        return;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "id", s->id);
    cJSON_AddStringToObject(resp, "session_id", s->id);
    cJSON_AddStringToObject(resp, "title", s->title);
    cJSON_AddStringToObject(resp, "created_at", s->created_at);
    char *str = cJSON_PrintUnformatted(resp);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(resp);
    session_free(s);
}

static void handle_chat(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    if (ctx->metrics) metrics_counter_inc(ctx->metrics, "echo_chat_requests_total", "Total chat requests");

    if (!middleware_check_unlock(req, ctx))
    {
        server_response_error(client, 401, "unauthorized");
        return;
    }

    if (!req->body || req->body_len == 0)
    {
        server_response_error(client, 400, "missing message");
        return;
    }

    cJSON *json = cJSON_Parse(req->body);
    if (!json)
    {
        server_response_error(client, 400, "invalid json");
        return;
    }

    cJSON *msg = cJSON_GetObjectItem(json, "message");
    if (!msg || !msg->valuestring)
    {
        cJSON_Delete(json);
        server_response_error(client, 400, "missing message field");
        return;
    }

    if (!ctx->agent)
    {
        cJSON_Delete(json);
        server_response_error(client, 500, "agent not initialized");
        return;
    }

    LLMResponse *resp = agent_run(ctx->agent, msg->valuestring);
    cJSON_Delete(json);

    if (!resp)
    {
        server_response_error(client, 500, "agent returned no response");
        return;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "content", resp->content ? resp->content : "");
    cJSON_AddBoolToObject(r, "has_tools", resp->tool_calls_count > 0);
    if (resp->thinking)
        cJSON_AddStringToObject(r, "thinking", resp->thinking);

    if (resp->tool_calls_count > 0)
    {
        cJSON *tc_arr = cJSON_CreateArray();
        for (int i = 0; i < resp->tool_calls_count; i++)
        {
            cJSON *tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "name", resp->tool_calls[i].name ? resp->tool_calls[i].name : "");
            cJSON_AddStringToObject(tc, "arguments", resp->tool_calls[i].arguments ? resp->tool_calls[i].arguments : "{}");
            cJSON_AddItemToArray(tc_arr, tc);
        }
        cJSON_AddItemToObject(r, "tool_calls", tc_arr);
    }

    char *str = cJSON_PrintUnformatted(r);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(r);
    llm_response_free(resp);
}

typedef struct {
    Agent *agent;
    Client *client;
} SSECtx;

static void sse_on_chunk(const char *chunk, void *userdata)
{
    SSECtx *c = (SSECtx *)userdata;
    if (!c || !c->client) return;
    char *sse = NULL;
    if (asprintf(&sse, "data: {\"type\":\"content\",\"content\":%s}\n\n",
                 chunk ? chunk : "") < 0) return;
    server_sse_write(c->client, sse);
    free(sse);
}

static void handle_sse_stream(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    if (!ctx->agent) { server_response_error(client, 500, "no agent"); return; }

    char *headers = NULL;
    if (asprintf(&headers,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n") < 0) return;

    server_sse_write(client, headers);
    free(headers);

    SSECtx *c = calloc(1, sizeof(SSECtx));
    if (!c) return;
    c->agent = ctx->agent;
    c->client = client;

    LLMResponse *resp = agent_run_streaming(ctx->agent, "", sse_on_chunk, c);
    free(c);

    if (resp)
    {
        server_sse_write(client, "data: {\"type\":\"done\"}\n\n");
        llm_response_free(resp);
    }
    else
    {
        server_sse_write(client, "data: {\"type\":\"error\",\"message\":\"no response\"}\n\n");
    }

    client_close(client);
}

static void handle_metrics(HTTPRequest *req, Client *client, ServerContext *ctx)
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

static void handle_undo(HTTPRequest *req, Client *client, ServerContext *ctx)
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

static void handle_redo(HTTPRequest *req, Client *client, ServerContext *ctx)
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

static void handle_change_password(HTTPRequest *req, Client *client, ServerContext *ctx)
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
        server_response_error(client, 500, "password change failed");
        return;
    }

    server_response_json(client, 200, "{\"changed\":true}");
}

static void handle_session_import(HTTPRequest *req, Client *client, ServerContext *ctx)
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

    Session *s = session_manager_import_session(ctx->sm, req->body);
    if (!s)
    {
        server_response_error(client, 400, "import failed — duplicate or invalid session data");
        return;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "id", s->id);
    cJSON_AddStringToObject(resp, "title", s->title);
    char *str = cJSON_PrintUnformatted(resp);
    server_response_json(client, 200, str);
    free(str);
    cJSON_Delete(resp);
    session_free(s);
}

static void handle_health_detailed(HTTPRequest *req, Client *client, ServerContext *ctx)
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

static void handle_models(HTTPRequest *req, Client *client, ServerContext *ctx)
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

static void handle_preferences_get(HTTPRequest *req, Client *client, ServerContext *ctx)
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

static void handle_preferences_set(HTTPRequest *req, Client *client, ServerContext *ctx)
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

const Route routes[] = {
    {"GET",  "/api/status",               0, 0, handle_status},
    {"GET",  "/api/health",               0, 0, handle_health},
    {"GET",  "/api/health/detailed",      0, 0, handle_health_detailed},
    {"GET",  "/api/config",               0, 0, handle_config},
    {"POST", "/api/setup",                0, 0, handle_setup},
    {"POST", "/api/unlock",               0, 0, handle_unlock},
    {"POST", "/api/logout",               0, 1, handle_logout},
    {"GET",  "/api/models",               0, 0, handle_models},
    {"GET",  "/api/preferences",          0, 0, handle_preferences_get},
    {"POST", "/api/preferences",          0, 0, handle_preferences_set},
    {"GET",  "/api/sessions",             0, 1, handle_sessions},
    {"POST", "/api/sessions",             0, 1, handle_create_session},
    {"POST", "/api/sessions/rename",      0, 1, handle_sessions_rename},
    {"GET",  "/api/sessions/",            1, 1, handle_session_get},
    {"DELETE","/api/sessions/",           1, 1, handle_session_delete},
    {"PUT",  "/api/sessions/",            1, 1, handle_session_update},
    {"POST", "/api/sessions/import",      0, 1, handle_session_import},
    {"POST", "/api/change-password",      0, 1, handle_change_password},
    {"POST", "/api/chat",                 0, 1, handle_chat},
    {"GET",  "/api/stream",               0, 0, handle_sse_stream},
    {"GET",  "/api/metrics",              0, 0, handle_metrics},
    {"POST", "/api/undo",                 0, 1, handle_undo},
    {"POST", "/api/redo",                 0, 1, handle_redo},
};

const int routes_count = sizeof(routes) / sizeof(routes[0]);

int route_match(const char *method, const char *path, const Route *r)
{
    if (strcmp(method, r->method) != 0) return 0;
    if (r->is_prefix)
        return strncmp(path, r->path, strlen(r->path)) == 0;
    return strcmp(path, r->path) == 0;
}

typedef struct QueuedMsg {
    char *data;
    struct QueuedMsg *next;
} QueuedMsg;

typedef struct {
    Agent *agent;
    SessionManager *sm;
    SafetyConfig *safety;
    WSClient *ws;
    uv_loop_t *loop;
    char *pending_request_id;
    int approval_done;
    int approval_result;
    int ready;
    QueuedMsg *msg_queue;
    QueuedMsg *msg_queue_tail;
    char *active_session_id;
    int session_start_emitted;
    int ask_user_done;
    char *ask_user_response;
} WSChatCtx;

static void ws_title_update_cb(const char *session_id, const char *title, void *userdata);

static void ws_chat_on_chunk(const char *chunk, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->ws) return;

    cJSON *frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "type", "content");
    cJSON_AddStringToObject(frame, "content", chunk ? chunk : "");
    if (c->active_session_id)
        cJSON_AddStringToObject(frame, "session_id", c->active_session_id);
    char *str = cJSON_PrintUnformatted(frame);
    if (str) ws_send_json(c->ws, str);
    free(str);
    cJSON_Delete(frame);
}

static void ws_send_done(WSClient *ws, const char *session_id, const char *title, LLMResponse *resp)
{
    cJSON *done = cJSON_CreateObject();
    cJSON_AddStringToObject(done, "type", "done");
    if (resp && resp->content)
        cJSON_AddStringToObject(done, "content", resp->content);
    if (session_id)
        cJSON_AddStringToObject(done, "session_id", session_id);
    if (title)
        cJSON_AddStringToObject(done, "title", title);
    if (resp)
    {
        cJSON_AddBoolToObject(done, "has_tools", resp->tool_calls_count > 0);
        if (resp->tool_calls_count > 0)
        {
            cJSON *tc_arr = cJSON_CreateArray();
            for (int i = 0; i < resp->tool_calls_count; i++)
            {
                cJSON *tc = cJSON_CreateObject();
                cJSON_AddStringToObject(tc, "name", resp->tool_calls[i].name ? resp->tool_calls[i].name : "");
                cJSON_AddStringToObject(tc, "arguments", resp->tool_calls[i].arguments ? resp->tool_calls[i].arguments : "{}");
                cJSON_AddItemToArray(tc_arr, tc);
            }
            cJSON_AddItemToObject(done, "tool_calls", tc_arr);
        }
    }
    char *str = cJSON_PrintUnformatted(done);
    if (str) ws_send_json(ws, str);
    free(str);
    cJSON_Delete(done);
}

static void ws_chat_flush_queue(WSChatCtx *c)
{
    if (!c || c->ready) return;
    c->ready = 1;

    QueuedMsg *q = c->msg_queue;
    while (q)
    {
        cJSON *json = cJSON_Parse(q->data);
        if (json)
        {
            cJSON *msg = cJSON_GetObjectItem(json, "content");
            if (!msg || !msg->valuestring)
                msg = cJSON_GetObjectItem(json, "message");
            if (msg && msg->valuestring)
            {
                LLMResponse *resp = agent_run_streaming(c->agent, msg->valuestring,
                                                        ws_chat_on_chunk, c);
                if (!c->active_session_id && c->agent && c->agent->session_id)
                {
                    free(c->active_session_id);
                    c->active_session_id = str_dup(c->agent->session_id);
                }
                if (resp)
                {
                    ws_send_done(c->ws, c->active_session_id, NULL, resp);
                    llm_response_free(resp);
                }
                else
                {
                    cJSON *err = cJSON_CreateObject();
                    cJSON_AddStringToObject(err, "type", "error");
                    cJSON_AddStringToObject(err, "content", "no response");
                    char *s = cJSON_PrintUnformatted(err);
                    if (s) ws_send_json(c->ws, s);
                    free(s);
                    cJSON_Delete(err);
                }
            }
            cJSON_Delete(json);
        }
        QueuedMsg *next = q->next;
        free(q->data);
        free(q);
        q = next;
    }
    c->msg_queue = NULL;
    c->msg_queue_tail = NULL;
}

static void ws_chat_enqueue(WSChatCtx *c, const char *data)
{
    QueuedMsg *q = calloc(1, sizeof(QueuedMsg));
    if (!q) return;
    q->data = str_dup(data);
    if (!q->data) { free(q); return; }

    if (c->msg_queue_tail)
        c->msg_queue_tail->next = q;
    else
        c->msg_queue = q;
    c->msg_queue_tail = q;
}

static void ws_chat_on_message(WSClient *ws, const char *data, size_t len, void *userdata)
{
    (void)len;
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->agent) return;

    cJSON *json = cJSON_Parse(data);
    if (!json)
    {
        ws_send_json(ws, "{\"type\":\"error\",\"message\":\"invalid json\"}");
        return;
    }

    cJSON *session_id_item = cJSON_GetObjectItem(json, "session_id");
    if (session_id_item && session_id_item->valuestring)
    {
        if (c->active_session_id)
        {
            if (strcmp(session_id_item->valuestring, c->active_session_id) != 0)
            {
                ws_send_json(ws, "{\"type\":\"error\",\"message\":\"stale session_id\"}");
                cJSON_Delete(json);
                return;
            }
        }
        else if (c->sm && c->agent)
        {
            Session *s = session_manager_load_session(c->sm, session_id_item->valuestring);
            if (s)
            {
                free(c->active_session_id);
                c->active_session_id = str_dup(session_id_item->valuestring);
                free(c->agent->session_id);
                c->agent->session_id = str_dup(session_id_item->valuestring);

                if (c->agent->messages)
                {
                    message_free_all(c->agent->messages, c->agent->messages_count);
                    c->agent->messages = NULL;
                    c->agent->messages_count = 0;
                }
                if (s->messages_count > 0)
                {
                    c->agent->messages = calloc((size_t)s->messages_count, sizeof(Message));
                    if (c->agent->messages)
                    {
                        for (int i = 0; i < s->messages_count; i++)
                        {
                            if (message_copy(&c->agent->messages[i], &s->messages[i]) != 0)
                            {
                                message_free_all(c->agent->messages, i);
                                c->agent->messages = NULL;
                                c->agent->messages_count = 0;
                                break;
                            }
                        }
                        if (c->agent->messages)
                            c->agent->messages_count = s->messages_count;
                    }

                    cJSON *hist = cJSON_CreateObject();
                    cJSON_AddStringToObject(hist, "type", "history");
                    cJSON *arr = cJSON_CreateArray();
                    for (int i = 0; i < s->messages_count; i++)
                    {
                        cJSON *m = cJSON_CreateObject();
                        ws_add_message_to_json(m, &s->messages[i]);
                        cJSON_AddItemToArray(arr, m);
                    }
                    cJSON_AddItemToObject(hist, "messages", arr);
                    char *hist_str = cJSON_PrintUnformatted(hist);
                    if (hist_str) ws_send_json(ws, hist_str);
                    free(hist_str);
                    cJSON_Delete(hist);
                }                session_free(s);
            }
        }
    }

    cJSON *type_item = cJSON_GetObjectItem(json, "type");

    if (!type_item)
    {
        cJSON *provider = cJSON_GetObjectItem(json, "provider");
        if (provider)
        {
            cJSON_Delete(json);
            cJSON *ready = cJSON_CreateObject();
            cJSON_AddStringToObject(ready, "type", "ready");
            if (c->active_session_id)
                cJSON_AddStringToObject(ready, "session_id", c->active_session_id);
            char *s = cJSON_PrintUnformatted(ready);
            if (s) ws_send_json(ws, s);
            free(s);
            cJSON_Delete(ready);
            return;
        }
        ws_send_json(ws, "{\"type\":\"error\",\"content\":\"missing type\"}");
        cJSON_Delete(json);
        return;
    }

    if (strcmp(type_item->valuestring, "approval_response") == 0)
        {
            cJSON *rid = cJSON_GetObjectItem(json, "request_id");
            cJSON *ok = cJSON_GetObjectItem(json, "approved");
            if (rid && rid->valuestring && ok && cJSON_IsBool(ok) &&
                c->pending_request_id && strcmp(rid->valuestring, c->pending_request_id) == 0)
            {
                c->approval_result = cJSON_IsTrue(ok) ? 1 : 0;
                c->approval_done = 1;
            }
            cJSON_Delete(json);
            return;
        }
        if (strcmp(type_item->valuestring, "ask_user_response") == 0)
        {
            cJSON *answer = cJSON_GetObjectItem(json, "answer");
            if (answer && answer->valuestring)
            {
                free(c->ask_user_response);
                c->ask_user_response = str_dup(answer->valuestring);
                c->ask_user_done = 1;
            }
            else
            {
                c->ask_user_response = str_dup("");
                c->ask_user_done = 1;
            }
            cJSON_Delete(json);
            return;
        }
        if (strcmp(type_item->valuestring, "edit") == 0)
        {
            cJSON *idx_item = cJSON_GetObjectItem(json, "index");
            cJSON *content_item = cJSON_GetObjectItem(json, "content");
            if (idx_item && cJSON_IsNumber(idx_item) && content_item && content_item->valuestring
                && c->sm && c->agent)
            {
                int idx = (int)idx_item->valuedouble;
                int trunc_rc = session_manager_truncate_history(c->sm, c->agent->session_id, idx);
                if (trunc_rc == 0 && c->agent->messages_count > 0)
                {
                    int keep = idx < c->agent->messages_count ? idx : c->agent->messages_count - 1;
                    for (int i = keep; i < c->agent->messages_count; i++)
                    {
                        free(c->agent->messages[i].role);
                        free(c->agent->messages[i].content);
                        free(c->agent->messages[i].id);
                        free(c->agent->messages[i].tool_call_id);
                        free(c->agent->messages[i].tool_name);
                        free(c->agent->messages[i].error_category);
                        free(c->agent->messages[i].thinking);
                        if (c->agent->messages[i].tool_calls)
                        {
                            for (int j = 0; j < c->agent->messages[i].tool_calls_count; j++)
                                tool_call_free(&c->agent->messages[i].tool_calls[j]);
                            free(c->agent->messages[i].tool_calls);
                        }
                    }
                    c->agent->messages_count = keep;
                }

                LLMResponse *resp = agent_run_streaming(c->agent, content_item->valuestring,
                                                        ws_chat_on_chunk, c);
                if (!c->active_session_id && c->agent && c->agent->session_id)
                {
                    free(c->active_session_id);
                    c->active_session_id = str_dup(c->agent->session_id);
                }
                if (resp)
                {
                    ws_send_done(c->ws, c->active_session_id, NULL, resp);
                    llm_response_free(resp);
                }
                else
                {
                    cJSON *err = cJSON_CreateObject();
                    cJSON_AddStringToObject(err, "type", "error");
                    cJSON_AddStringToObject(err, "content", "no response");
                    char *s = cJSON_PrintUnformatted(err);
                    if (s) ws_send_json(c->ws, s);
                    free(s);
                    cJSON_Delete(err);
                }
            }
            cJSON_Delete(json);
            return;
        }

        if (strcmp(type_item->valuestring, "stop") == 0)
        {
            agent_cancel(c->agent);
            c->approval_done = 1;
            c->ask_user_done = 1;
            cJSON_Delete(json);
            return;
        }

        if (strcmp(type_item->valuestring, "message") != 0)
        {
            cJSON_Delete(json);
            return;
        }

    cJSON *msg = cJSON_GetObjectItem(json, "content");
    if (!msg || !msg->valuestring)
        msg = cJSON_GetObjectItem(json, "message");
    if (!msg || !msg->valuestring)
    {
        cJSON_Delete(json);
        ws_send_json(ws, "{\"type\":\"error\",\"content\":\"missing message content\"}");
        return;
    }

    if (!c->ready)
    {
        ws_chat_enqueue(c, data);
        cJSON_Delete(json);
        return;
    }

    LLMResponse *resp = agent_run_streaming(c->agent, msg->valuestring,
                                            ws_chat_on_chunk, c);
    cJSON_Delete(json);

    /* agent may have created a session during the run — capture it */
    if (!c->active_session_id && c->agent && c->agent->session_id)
    {
        free(c->active_session_id);
        c->active_session_id = str_dup(c->agent->session_id);
    }

    if (resp)
    {
        ws_send_done(ws, c->active_session_id, NULL, resp);
        llm_response_free(resp);
    }
    else
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "type", "error");
        cJSON_AddStringToObject(err, "content", "agent returned no response");
        char *s = cJSON_PrintUnformatted(err);
        if (s) ws_send_json(ws, s);
        free(s);
        cJSON_Delete(err);
    }
}

static void ws_title_update_cb(const char *session_id, const char *title, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->ws) return;

    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", "title_updated");
    cJSON_AddStringToObject(ev, "session_id", session_id ? session_id : "");
    cJSON_AddStringToObject(ev, "title", title ? title : "");
    char *str = cJSON_PrintUnformatted(ev);
    if (str) ws_send_json(c->ws, str);
    free(str);
    cJSON_Delete(ev);
}

static void ws_tool_start_cb(const char *tool_name, const char *arguments, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->ws) return;

    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", "tool_start");
    cJSON_AddStringToObject(ev, "tool_name", tool_name ? tool_name : "");
    cJSON_AddStringToObject(ev, "arguments", arguments ? arguments : "{}");
    char *str = cJSON_PrintUnformatted(ev);
    if (str) ws_send_json(c->ws, str);
    free(str);
    cJSON_Delete(ev);
}

static void ws_chat_on_close(WSClient *ws, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (c)
    {
        ws->on_close = NULL;
        ws->userdata = NULL;
        c->approval_done = 1;
        c->approval_result = 0;
        if (c->agent) agent_cancel(c->agent);
        free(c->pending_request_id);
        free(c->active_session_id);
        QueuedMsg *q = c->msg_queue;
        while (q)
        {
            QueuedMsg *next = q->next;
            free(q->data);
            free(q);
            q = next;
        }
        free(c);
    }
}

static int ws_approval_cb(const char *tool_name, const char *arguments, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->ws) return 0;

    char *req_id = NULL;
    static unsigned long approval_counter = 0;
    approval_counter++;
    if (asprintf(&req_id, "apr_%lu", approval_counter) < 0) return 0;

    free(c->pending_request_id);
    c->pending_request_id = str_dup(req_id);
    c->approval_done = 0;
    c->approval_result = 0;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "approval_request");
    cJSON_AddStringToObject(req, "request_id", req_id);
    cJSON_AddStringToObject(req, "tool_name", tool_name);
    cJSON_AddStringToObject(req, "arguments", arguments);
    char *req_str = cJSON_PrintUnformatted(req);
    if (req_str) ws_send_json(c->ws, req_str);
    free(req_str);
    cJSON_Delete(req);

    while (!c->approval_done && c->ws && c->loop)
        uv_run(c->loop, UV_RUN_NOWAIT);

    int approved = c->approval_result;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "approval_response");
    cJSON_AddStringToObject(resp, "request_id", req_id);
    cJSON_AddBoolToObject(resp, "approved", approved);
    char *resp_str = cJSON_PrintUnformatted(resp);
    if (resp_str) ws_send_json(c->ws, resp_str);
    free(resp_str);
    cJSON_Delete(resp);
    free(req_id);

    return approved;
}

static char *ws_ask_user_cb(const char *question, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->ws) return NULL;

    c->ask_user_done = 0;
    free(c->ask_user_response);
    c->ask_user_response = NULL;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "ask_user");
    cJSON_AddStringToObject(req, "question", question ? question : "");
    char *req_str = cJSON_PrintUnformatted(req);
    if (req_str) ws_send_json(c->ws, req_str);
    free(req_str);
    cJSON_Delete(req);

    while (!c->ask_user_done && c->ws && c->loop)
        uv_run(c->loop, UV_RUN_NOWAIT);

    char *answer = c->ask_user_response ? str_dup(c->ask_user_response) : NULL;
    return answer;
}

static void ws_chat_emit_session_start(WSChatCtx *c)
{
    if (!c->agent || !c->agent->session_id) return;
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", "session_start");
    cJSON_AddStringToObject(ev, "session_id", c->agent->session_id);
    char *str = cJSON_PrintUnformatted(ev);
    if (str) ws_send_json(c->ws, str);
    free(str);
    cJSON_Delete(ev);
    c->session_start_emitted = 1;
}

void routes_ws_chat_init(WSClient *ws, ServerContext *ctx, const char *query)
{
    WSChatCtx *c = calloc(1, sizeof(WSChatCtx));
    if (!c) return;
    c->agent = ctx->agent;
    c->sm = ctx->sm;
    c->safety = ctx->safety;
    c->ws = ws;
    c->loop = ctx->loop;

    ws->on_message = ws_chat_on_message;
    ws->on_close = ws_chat_on_close;
    ws->userdata = c;

    if (c->agent)
    {
        agent_set_approval_callback(c->agent, ws_approval_cb, c);
        agent_set_title_callback(c->agent, ws_title_update_cb, c);
        agent_set_tool_start_callback(c->agent, ws_tool_start_cb, c);
    }
    registry_set_ask_user_callback(ws_ask_user_cb, c);

    if (query && query[0] && c->sm && c->agent)
    {
        const char *sid_start = strstr(query, "session_id=");
        if (sid_start)
        {
            sid_start += 11;
            const char *sid_end = strchr(sid_start, '&');
            size_t sid_len = sid_end ? (size_t)(sid_end - sid_start) : strlen(sid_start);
            if (sid_len > 0 && sid_len < 256)
            {
                char session_id[256];
                memcpy(session_id, sid_start, sid_len);
                session_id[sid_len] = '\0';

                free(c->active_session_id);
                c->active_session_id = str_dup(session_id);

                Session *s = session_manager_load_session(c->sm, session_id);
                if (s)
                {
                    free(c->agent->session_id);
                    c->agent->session_id = str_dup(session_id);

                    if (s->messages_count > 0)
                    {
                        cJSON *hist = cJSON_CreateObject();
                        cJSON_AddStringToObject(hist, "type", "history");
                        cJSON *arr = cJSON_CreateArray();
                        for (int i = 0; i < s->messages_count; i++)
                        {
                            cJSON *m = cJSON_CreateObject();
                            ws_add_message_to_json(m, &s->messages[i]);
                            cJSON_AddItemToArray(arr, m);
                        }
                        cJSON_AddItemToObject(hist, "messages", arr);
                        char *hist_str = cJSON_PrintUnformatted(hist);
                        if (hist_str) ws_send_json(ws, hist_str);
                        free(hist_str);
                        cJSON_Delete(hist);
                    }

                    session_free(s);
                }
            }
        }
    }

    if (!c->active_session_id && c->agent && c->agent->session_id)
        c->active_session_id = str_dup(c->agent->session_id);

    ws_chat_emit_session_start(c);

    cJSON *ready = cJSON_CreateObject();
    cJSON_AddStringToObject(ready, "type", "ready");
    if (c->agent && c->agent->session_id)
        cJSON_AddStringToObject(ready, "session_id", c->agent->session_id);
    char *ready_str = cJSON_PrintUnformatted(ready);
    if (ready_str) ws_send_json(ws, ready_str);
    free(ready_str);
    cJSON_Delete(ready);

    ws_chat_flush_queue(c);
}
