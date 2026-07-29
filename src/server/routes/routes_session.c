#include <stdlib.h>
#include <string.h>
#ifndef ROUTES_SESSION_TEST
#include <cjson/cJSON.h>
#endif

#include "routes.h"
#include "routes_session.h"
#ifndef ROUTES_SESSION_TEST
#include "../middleware.h"
#include "../../session/session_manager.h"
#include "../../utils/logging.h"
#include "../../utils/string_utils.h"
#endif

#ifndef ROUTES_SESSION_TEST
void handle_sessions(HTTPRequest *req, Client *client, ServerContext *ctx)
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
        /* G1: previously list->title_generation_attempteds was populated
         * by session_manager_list_sessions but never surfaced — pure dead
         * data allocation. Now emitted so the field isn't wasted work. */
        cJSON_AddBoolToObject(s, "title_generation_attempted",
                              list->title_generation_attempteds[i]);
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

void handle_create_session(HTTPRequest *req, Client *client, ServerContext *ctx)
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

#endif /* ROUTES_SESSION_TEST */

#ifdef ROUTES_SESSION_TEST
const char *session_id_from_path(const char *path)
#else
static const char *session_id_from_path(const char *path)
#endif
{
    const char *prefix = "/api/sessions/";
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) == 0 && strlen(path) > plen)
        return path + plen;
    return NULL;
}

#ifdef ROUTES_SESSION_TEST
int is_export_path(const char *sid)
#else
static int is_export_path(const char *sid)
#endif
{
    size_t slen = strlen(sid);
    return (slen > 7 && strcmp(sid + slen - 7, "/export") == 0)
        || (slen > 13 && strcmp(sid + slen - 13, "/debug-export") == 0);
}

#ifdef ROUTES_SESSION_TEST
size_t export_suffix_len(const char *sid)
#else
static size_t export_suffix_len(const char *sid)
#endif
{
    size_t slen = strlen(sid);
    if (slen > 13 && strcmp(sid + slen - 13, "/debug-export") == 0) return 13;
    if (slen > 7 && strcmp(sid + slen - 7, "/export") == 0) return 7;
    return 0;
}

#ifndef ROUTES_SESSION_TEST
void handle_session_get(HTTPRequest *req, Client *client, ServerContext *ctx)
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
        size_t slen = export_suffix_len(sid);
        id_copy[strlen(id_copy) - slen] = '\0';
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

void handle_session_delete(HTTPRequest *req, Client *client, ServerContext *ctx)
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

    int del_rc = session_manager_delete_session(ctx->sm, sid);
    if (del_rc < 0)
    {
        server_response_error(client, 500, "delete failed");
        return;
    }
    if (del_rc == 0)
    {
        server_response_error(client, 404, "session not found");
        return;
    }

    server_response_json(client, 200, "{\"deleted\":true}");
}

void handle_session_update(HTTPRequest *req, Client *client, ServerContext *ctx)
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

void handle_sessions_rename(HTTPRequest *req, Client *client, ServerContext *ctx)
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

void handle_session_import(HTTPRequest *req, Client *client, ServerContext *ctx)
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

#endif /* ROUTES_SESSION_TEST */
