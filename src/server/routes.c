#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cjson/cJSON.h>

#include "routes.h"
#include "websocket.h"
#include "middleware.h"
#include "../agent/agent.h"
#include "../safety/safety.h"
#include "../session/encryption.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

static void handle_status(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "locked", ctx->state == STATE_LOCKED);
    cJSON_AddBoolToObject(json, "needs_setup", ctx->state == STATE_SETUP);
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
    if (!ctx->config_path)
    {
        server_response_error(client, 500, "no config path");
        return;
    }

    FILE *f = fopen(ctx->config_path, "rb");
    if (!f)
    {
        server_response_error(client, 500, "cannot read config");
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    if (fsize <= 0 || fsize > 65536) { fclose(f); server_response_error(client, 500, "config too large"); return; }

    char *buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); server_response_error(client, 500, "oom"); return; }

    size_t read = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[read] = '\0';

    server_response(client, 200, "text/plain", buf);
    free(buf);
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
    cJSON_Delete(json);

    const char *home = getenv("HOME");
    if (!home) { server_response_error(client, 500, "HOME not set"); return; }

    char *salt_path = NULL;
    if (asprintf(&salt_path, "%s/.config/echo-ai/salt", home) < 0)
    { server_response_error(client, 500, "out of memory"); return; }

    if (encryption_salt_create(salt_path) != 0)
    {
        free(salt_path);
        server_response_error(client, 500, "failed to create salt");
        return;
    }
    free(salt_path);

    char token[64];
    snprintf(token, sizeof(token), "tok_%ld_%d", (long)time(NULL), rand() % 100000);
    ctx->unlock_token = str_dup(token);
    ctx->state = STATE_UNLOCKED;

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
    if (ctx->state != STATE_LOCKED)
    {
        server_response_error(client, 400, "not locked");
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
        free(salt_path);
        free(password);
        server_response_error(client, 401, "wrong password");
        return;
    }

    memset(&key, 0, sizeof(key));
    free(salt_path);
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
    cJSON_AddStringToObject(resp, "id", s->id);
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

    Session *s = session_manager_load_session(ctx->sm, sid);
    if (!s)
    {
        server_response_error(client, 404, "session not found");
        return;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "id", s->id);
    cJSON_AddStringToObject(resp, "title", s->title);
    cJSON_AddStringToObject(resp, "created_at", s->created_at);
    if (s->messages_count > 0)
    {
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < s->messages_count; i++)
        {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", s->messages[i].role ? s->messages[i].role : "unknown");
            cJSON_AddStringToObject(m, "content", s->messages[i].content ? s->messages[i].content : "");
            cJSON_AddItemToArray(arr, m);
        }
        cJSON_AddItemToObject(resp, "messages", arr);
    }
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

const Route routes[] = {
    {"GET",  "/api/status",        0, 0, handle_status},
    {"GET",  "/api/health",        0, 0, handle_health},
    {"GET",  "/api/config",        0, 0, handle_config},
    {"POST", "/api/setup",         0, 0, handle_setup},
    {"POST", "/api/unlock",        0, 0, handle_unlock},
    {"POST", "/api/logout",        0, 1, handle_logout},
    {"GET",  "/api/sessions",      0, 1, handle_sessions},
    {"POST", "/api/sessions",      0, 1, handle_create_session},
    {"GET",  "/api/sessions/",     1, 1, handle_session_get},
    {"DELETE", "/api/sessions/",   1, 1, handle_session_delete},
    {"PUT",  "/api/sessions/",     1, 1, handle_session_update},
    {"POST", "/api/chat",          0, 1, handle_chat},
    {"GET",  "/api/stream",        0, 0, handle_sse_stream},
    {"GET",  "/api/metrics",       0, 0, handle_metrics},
    {"POST", "/api/undo",          0, 1, handle_undo},
    {"POST", "/api/redo",          0, 1, handle_redo},
};

const int routes_count = sizeof(routes) / sizeof(routes[0]);

int route_match(const char *method, const char *path, const Route *r)
{
    if (strcmp(method, r->method) != 0) return 0;
    if (r->is_prefix)
        return strncmp(path, r->path, strlen(r->path)) == 0;
    return strcmp(path, r->path) == 0;
}

typedef struct {
    Agent *agent;
    SessionManager *sm;
    SafetyConfig *safety;
    WSClient *ws;
    uv_loop_t *loop;
    char *pending_request_id;
    int approval_done;
    int approval_result;
} WSChatCtx;

static void ws_chat_on_chunk(const char *chunk, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->ws) return;

    cJSON *frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "type", "content");
    cJSON_AddStringToObject(frame, "content", chunk ? chunk : "");
    char *str = cJSON_PrintUnformatted(frame);
    if (str) ws_send_json(c->ws, str);
    free(str);
    cJSON_Delete(frame);
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

    cJSON *type_item = cJSON_GetObjectItem(json, "type");
    if (type_item && type_item->valuestring)
    {
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
        if (strcmp(type_item->valuestring, "stop") == 0)
        {
            agent_cancel(c->agent);
            c->approval_done = 1;
            cJSON_Delete(json);
            return;
        }
        if (strcmp(type_item->valuestring, "message") != 0)
        {
            cJSON_Delete(json);
            return;
        }
    }

    cJSON *msg = cJSON_GetObjectItem(json, "message");
    if (!msg || !msg->valuestring)
    {
        cJSON_Delete(json);
        ws_send_json(ws, "{\"type\":\"error\",\"message\":\"missing message field\"}");
        return;
    }

    LLMResponse *resp = agent_run_streaming(c->agent, msg->valuestring,
                                            ws_chat_on_chunk, c);
    cJSON_Delete(json);

    if (resp)
    {
        cJSON *done = cJSON_CreateObject();
        cJSON_AddStringToObject(done, "type", "done");
        char *str = cJSON_PrintUnformatted(done);
        if (str) ws_send_json(ws, str);
        free(str);
        cJSON_Delete(done);
        llm_response_free(resp);
    }
    else
    {
        ws_send_json(ws, "{\"type\":\"error\",\"message\":\"agent returned no response\"}");
    }
}

static void ws_chat_on_close(WSClient *ws, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    (void)ws;
    if (c)
    {
        c->approval_done = 1;
        free(c->pending_request_id);
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
        agent_set_approval_callback(c->agent, ws_approval_cb, c);

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
                            cJSON_AddStringToObject(m, "role",
                                s->messages[i].role ? s->messages[i].role : "unknown");
                            cJSON_AddStringToObject(m, "content",
                                s->messages[i].content ? s->messages[i].content : "");
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

    cJSON *ready = cJSON_CreateObject();
    cJSON_AddStringToObject(ready, "type", "ready");
    if (c->agent && c->agent->session_id)
        cJSON_AddStringToObject(ready, "session_id", c->agent->session_id);
    char *ready_str = cJSON_PrintUnformatted(ready);
    if (ready_str) ws_send_json(ws, ready_str);
    free(ready_str);
    cJSON_Delete(ready);
}
