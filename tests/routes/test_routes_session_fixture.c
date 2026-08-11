/*
 * test_routes_session_fixture.c - shared stubs and fixtures for the
 * routes_session test binaries (handlers/get/mutate/import): stub
 * state, server/middleware/session stubs, and context builders.
 * Split from test_routes_session_handlers.c (2026-08 file-length
 * compliance). Depends on: check, routes_session.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "test_routes_session_fixture.h"

/* test_routes_session_handlers - unit tests for routes session handlers. Depends on: check, the module under test. */
/* ---------------------------------------------------------------------------
 * Stub state — controlling return values for mocked functions
 * --------------------------------------------------------------------------- */

int stub_unlock_result = 1;
int stub_list_result_null = 0;
int stub_list_count = 0;
char *stub_list_ids[4] = {0};
char *stub_list_titles[4] = {0};
char *stub_list_created_ats[4] = {0};
int stub_list_title_gens[4] = {0};
Session *stub_create_result = NULL;
Session *stub_load_result = NULL;
int stub_save_result = 0;
int stub_delete_result = 1;
int stub_import_result_null = 0;
char *stub_export_result = NULL;
int captured_status = 0;
char *captured_body = NULL;

void reset_capture(void)
{
    captured_status = 0;
    free(captured_body);
    captured_body = NULL;
}

void reset_stubs(void)
{
    stub_unlock_result = 1;
    stub_list_result_null = 0;
    stub_list_count = 0;
    stub_create_result = NULL;
    stub_load_result = NULL;
    stub_save_result = 0;
    stub_delete_result = 1;
    stub_import_result_null = 0;
    stub_export_result = NULL;
    reset_capture();
    for (int i = 0; i < 4; i++)
    {
        stub_list_ids[i] = NULL;
        stub_list_titles[i] = NULL;
        stub_list_created_ats[i] = NULL;
        stub_list_title_gens[i] = 0;
    }
}

/* ---------------------------------------------------------------------------
 * Stub server functions
 * --------------------------------------------------------------------------- */

int server_response(Client *client, int status, const char *content_type,
                     const char *body)
{
    (void)client; (void)content_type;
    captured_status = status;
    free(captured_body);
    captured_body = body ? str_dup(body) : NULL;
    return 0;
}

int server_response_json(Client *client, int status, const char *json)
{
    captured_status = status;
    free(captured_body);
    captured_body = json ? str_dup(json) : NULL;
    (void)client;
    return 0;
}

int server_response_error(Client *client, int status, const char *msg)
{
    captured_status = status;
    free(captured_body);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg ? msg : "");
    captured_body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    (void)client;
    return 0;
}

void client_close(Client *client) { (void)client; }
int server_sse_write(Client *client, const char *data)
 {
    (void)client;
    (void)data;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Stub middleware
 * --------------------------------------------------------------------------- */

int middleware_check_unlock(HTTPRequest *req, ServerContext *ctx)
{
    (void)req; (void)ctx;
    return stub_unlock_result;
}

int middleware_has_valid_token(const char *headers, const char *token)
{
    (void)headers; (void)token;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Stub ws_add_message_to_json (from routes.c)
 * --------------------------------------------------------------------------- */

int ws_add_message_to_json(cJSON *m, const Message *msg)
{
    (void)m; (void)msg;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Stub session_manager functions
 * --------------------------------------------------------------------------- */

SessionList *session_manager_list_sessions(SessionManager *sm)
{
    (void)sm;
    if (stub_list_result_null) return NULL;
    SessionList *list = calloc(1, sizeof(SessionList));
    if (!list) return NULL;
    list->count = stub_list_count;
    if (stub_list_count > 0)
    {
        list->ids = calloc((size_t)stub_list_count, sizeof(char *));
        list->titles = calloc((size_t)stub_list_count, sizeof(char *));
        list->created_ats = calloc((size_t)stub_list_count, sizeof(char *));
        list->title_generation_attempteds = calloc((size_t)stub_list_count, sizeof(int));
        if (!list->ids || !list->titles || !list->created_ats || !list->title_generation_attempteds)
        {
            free(list->ids); free(list->titles); free(list->created_ats);
            free(list->title_generation_attempteds); free(list);
            return NULL;
        }
        for (int i = 0; i < stub_list_count; i++)
        {
            list->ids[i] = str_dup(stub_list_ids[i] ? stub_list_ids[i] : "");
            list->titles[i] = str_dup(stub_list_titles[i] ? stub_list_titles[i] : "");
            list->created_ats[i] = str_dup(stub_list_created_ats[i] ? stub_list_created_ats[i] : "");
            list->title_generation_attempteds[i] = stub_list_title_gens[i];
        }
    }
    return list;
}

void session_list_free(SessionList *list)
{
    if (!list) return;
    for (int i = 0; i < list->count; i++)
    {
        free(list->ids[i]);
        free(list->titles[i]);
        free(list->created_ats[i]);
    }
    free(list->ids);
    free(list->titles);
    free(list->created_ats);
    free(list->title_generation_attempteds);
    free(list);
}

Session *session_manager_create_session(SessionManager *sm, const char *title)
{
    (void)sm; (void)title;
    if (stub_create_result)
    {
        Session *s = calloc(1, sizeof(Session));
        if (s)
        {
            s->id = str_dup(stub_create_result->id);
            s->title = str_dup(stub_create_result->title);
            s->created_at = str_dup(stub_create_result->created_at);
        }
        return s;
    }
    return NULL;
}

Session *session_manager_load_session_alloc(SessionManager *sm, const char *id)
{
    (void)sm; (void)id;
    if (stub_load_result)
    {
        Session *s = calloc(1, sizeof(Session));
        if (s)
        {
            s->id = str_dup(stub_load_result->id);
            s->title = str_dup(stub_load_result->title);
            s->created_at = str_dup(stub_load_result->created_at);
        }
        return s;
    }
    return NULL;
}

int session_manager_save_session(SessionManager *sm, Session *session)
{
    (void)sm; (void)session;
    return stub_save_result;
}

int session_manager_delete_session(SessionManager *sm, const char *id)
{
    (void)sm; (void)id;
    return stub_delete_result;
}

char *session_manager_export_session_new(SessionManager *sm, const char *session_id)
{
    (void)sm; (void)session_id;
    return stub_export_result ? str_dup(stub_export_result) : NULL;
}

Session *session_manager_import_session_new(SessionManager *sm, const char *json_str)
{
    (void)sm; (void)json_str;
    if (stub_import_result_null) return NULL;
    Session *s = calloc(1, sizeof(Session));
    if (s) {
        s->id = str_dup("imp-123");
        s->title = str_dup("Imported Session");
    }
    return s;
}

int session_manager_truncate_history(SessionManager *sm, const char *session_id,
                                      int index)
 {
    (void)sm;
    (void)session_id;
    (void)index;
    return 0;
}

const char *stub_branch_info_json = NULL;
char *session_manager_branch_info_alloc(SessionManager *sm, const char *sid)
{
    (void)sm; (void)sid;
    return str_dup(stub_branch_info_json ? stub_branch_info_json : "[]");
}

void session_manager_lock(SessionManager *sm) { (void)sm; }
void session_manager_unlock(SessionManager *sm) { (void)sm; }

/* ---------------------------------------------------------------------------
 * Stub session_free + message_clear
 * --------------------------------------------------------------------------- */

void message_clear(Message *msg)
{
    if (msg)
    {
        free(msg->role);
        free(msg->content);
        free(msg->id);
        free(msg->parent_id);
        free(msg->fork_group_id);
        free(msg->tool_call_id);
        free(msg->tool_name);
        free(msg->error_category);
        free(msg->thinking);
        if (msg->tool_calls)
        {
            for (int i = 0; i < msg->tool_calls_count; i++)
            {
                free(msg->tool_calls[i].id);
                free(msg->tool_calls[i].name);
                free(msg->tool_calls[i].arguments);
                free(msg->tool_calls[i].result_content);
                free(msg->tool_calls[i].result_error);
            }
            free(msg->tool_calls);
        }
        memset(msg, 0, sizeof(Message));
    }
}

void session_free(Session *s)
{
    if (!s) return;
    free(s->id);
    free(s->title);
    free(s->created_at);
    if (s->messages)
    {
        for (int i = 0; i < s->messages_count; i++)
            message_clear(&s->messages[i]);
        free(s->messages);
    }
    if (s->metadata) cJSON_Delete(s->metadata);
    if (s->events) cJSON_Delete(s->events);
    free(s);
}

/* ---------------------------------------------------------------------------
 * Helper to build a fake ServerContext
 * --------------------------------------------------------------------------- */

ServerContext make_ctx(SessionManager *sm, ServerState state,
                               const char *token)
{
    ServerContext ctx = {0};
    ctx.sm = sm;
    ctx.state = state;
    ctx.unlock_token = token ? (char *)token : NULL;
    return ctx;
}

HTTPRequest make_req(const char *path, const char *body,
                             const char *headers)
{
    HTTPRequest req = {0};
    if (path) strncpy(req.path, path, sizeof(req.path) - 1);
    if (body) {
        req.body = str_dup(body);
        req.body_len = strlen(body);
    }
    if (headers) strncpy(req.headers, headers, sizeof(req.headers) - 1);
    return req;
}

void free_req(HTTPRequest *req)
{
    free(req->body);
}
void setup(void) { reset_stubs(); }
void teardown(void) { reset_stubs(); }
