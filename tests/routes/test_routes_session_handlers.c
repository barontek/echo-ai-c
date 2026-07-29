#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "../src/server/routes/routes.h"
#include "../src/server/routes/routes_session.h"
#include "../src/utils/string_utils.h"

/* ---------------------------------------------------------------------------
 * Stub state — controlling return values for mocked functions
 * --------------------------------------------------------------------------- */

static int stub_unlock_result = 1;
static int stub_list_result_null = 0;
static int stub_list_count = 0;
static char *stub_list_ids[4] = {0};
static char *stub_list_titles[4] = {0};
static char *stub_list_created_ats[4] = {0};
static int stub_list_title_gens[4] = {0};
static Session *stub_create_result = NULL;
static Session *stub_load_result = NULL;
static int stub_save_result = 0;
static int stub_delete_result = 1;
static int stub_import_result_null = 0;
static char *stub_export_result = NULL;
static int captured_status = 0;
static char *captured_body = NULL;

static void reset_capture(void)
{
    captured_status = 0;
    free(captured_body);
    captured_body = NULL;
}

static void reset_stubs(void)
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

void server_response(Client *client, int status, const char *content_type,
                     const char *body)
{
    (void)client; (void)content_type;
    captured_status = status;
    free(captured_body);
    captured_body = body ? str_dup(body) : NULL;
}

void server_response_json(Client *client, int status, const char *json)
{
    captured_status = status;
    free(captured_body);
    captured_body = json ? str_dup(json) : NULL;
    (void)client;
}

void server_response_error(Client *client, int status, const char *msg)
{
    captured_status = status;
    free(captured_body);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg ? msg : "");
    captured_body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    (void)client;
}

void client_close(Client *client) { (void)client; }
void server_sse_write(Client *client, const char *data)
{ (void)client; (void)data; }

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

void ws_add_message_to_json(cJSON *m, const Message *msg)
{
    (void)m; (void)msg;
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

Session *session_manager_load_session(SessionManager *sm, const char *id)
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

char *session_manager_export_session(SessionManager *sm, const char *session_id)
{
    (void)sm; (void)session_id;
    return stub_export_result ? str_dup(stub_export_result) : NULL;
}

Session *session_manager_import_session(SessionManager *sm, const char *json_str)
{
    (void)sm; (void)json_str;
    if (stub_import_result_null) return NULL;
    Session *s = calloc(1, sizeof(Session));
    if (s) { s->id = str_dup("imp-123"); s->title = str_dup("Imported Session"); }
    return s;
}

int session_manager_truncate_history(SessionManager *sm, const char *session_id,
                                      int index)
{ (void)sm; (void)session_id; (void)index; return 0; }

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

static ServerContext make_ctx(SessionManager *sm, ServerState state,
                               const char *token)
{
    ServerContext ctx = {0};
    ctx.sm = sm;
    ctx.state = state;
    ctx.unlock_token = token ? (char *)token : NULL;
    return ctx;
}

static HTTPRequest make_req(const char *path, const char *body,
                             const char *headers)
{
    HTTPRequest req = {0};
    if (path) strncpy(req.path, path, sizeof(req.path) - 1);
    if (body) { req.body = str_dup(body); req.body_len = strlen(body); }
    if (headers) strncpy(req.headers, headers, sizeof(req.headers) - 1);
    return req;
}

static void free_req(HTTPRequest *req)
{
    free(req->body);
}

/* ---------------------------------------------------------------------------
 * handle_sessions
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_sessions_no_unlock)
{
    stub_unlock_result = 0;
    ServerContext ctx = make_ctx(NULL, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions", NULL, NULL);

    handle_sessions(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 401);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_sessions_no_sm)
{
    stub_unlock_result = 1;
    ServerContext ctx = make_ctx(NULL, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions", NULL, NULL);

    handle_sessions(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(captured_body && strstr(captured_body, "\"sessions\":[]"));
    ck_assert(strstr(captured_body, "\"session_enabled\":false"));

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_sessions_list_null)
{
    stub_unlock_result = 1;
    stub_list_result_null = 1;
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions", NULL, NULL);

    handle_sessions(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(captured_body && strstr(captured_body, "\"sessions\":[]"));

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_sessions_with_data)
{
    stub_unlock_result = 1;
    stub_list_count = 1;
    stub_list_ids[0] = "s1";
    stub_list_titles[0] = "Chat 1";
    stub_list_created_ats[0] = "2024-01-01";
    stub_list_title_gens[0] = 1;

    SessionManager sm = {0};
    HTTPRequest *req = calloc(1, sizeof(HTTPRequest));
    ck_assert_ptr_nonnull(req);
    strncpy(req->path, "/api/sessions", sizeof(req->path) - 1);
    ServerContext *ctx = calloc(1, sizeof(ServerContext));
    ck_assert_ptr_nonnull(ctx);
    ctx->sm = &sm;
    ctx->state = STATE_UNLOCKED;

    handle_sessions(req, NULL, ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"id\":\"s1\""));
    ck_assert(strstr(captured_body, "\"title\":\"Chat 1\""));
    ck_assert(strstr(captured_body, "\"title_generation_attempted\":true"));

    reset_stubs(); free(req); free(ctx);
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_create_session
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_create_session_no_unlock)
{
    stub_unlock_result = 0;
    ServerContext ctx = make_ctx(NULL, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions", NULL, NULL);

    handle_create_session(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 401);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_create_session_no_sm)
{
    stub_unlock_result = 1;
    ServerContext ctx = make_ctx(NULL, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions", NULL, NULL);

    handle_create_session(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_create_session_default_title)
{
    stub_unlock_result = 1;
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions", NULL, NULL);

    Session s = {0};
    s.id = "new-session";
    s.title = "Chat Session";
    s.created_at = "now";
    stub_create_result = &s;

    handle_create_session(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"title\":\"Chat Session\""));

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_create_session_custom_title)
{
    stub_unlock_result = 1;
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions", "{\"title\":\"Custom Chat\"}",
                                NULL);

    Session s = {0};
    s.id = "new-session";
    s.title = "Custom Chat";
    s.created_at = "now";
    stub_create_result = &s;

    handle_create_session(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"title\":\"Custom Chat\""));

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_create_session_invalid_json_still_defaults)
{
    stub_unlock_result = 1;
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions", "not json", NULL);

    Session s = {0};
    s.id = "new-session";
    s.title = "Chat Session";
    s.created_at = "now";
    stub_create_result = &s;

    handle_create_session(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"title\":\"Chat Session\""));

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_create_session_fails)
{
    stub_unlock_result = 1;
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions", NULL, NULL);

    stub_create_result = NULL;

    handle_create_session(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    reset_stubs(); free_req(&req);
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_session_get
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_session_get_no_sm)
{
    ServerContext ctx = make_ctx(NULL, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc", NULL, NULL);

    handle_session_get(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_get_no_sid)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/", NULL, NULL);

    handle_session_get(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_get_not_found)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/nonexistent", NULL, NULL);

    stub_load_result = NULL;

    handle_session_get(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 404);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_get_export)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc-123/export", NULL, NULL);

    stub_export_result = "{\"exported\":true}";

    handle_session_get(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"exported\":true"));

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_get_export_not_found)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc-123/export", NULL, NULL);

    stub_export_result = NULL;

    handle_session_get(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 404);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_get_debug_export)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc-123/debug-export", NULL, NULL);

    stub_export_result = "{\"debug\":true}";

    handle_session_get(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"debug\":true"));

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_get_success)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc-123", NULL, NULL);

    Session s = {0};
    s.id = "abc-123";
    s.title = "Chat Session";
    s.created_at = "2024-01-01";
    stub_load_result = &s;

    handle_session_get(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"id\":\"abc-123\""));
    ck_assert(strstr(captured_body, "\"session_id\":\"abc-123\""));
    ck_assert(strstr(captured_body, "\"title\":\"Chat Session\""));

    reset_stubs(); free_req(&req);
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_session_delete
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_session_delete_no_sm)
{
    ServerContext ctx = make_ctx(NULL, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc", NULL, NULL);

    handle_session_delete(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_delete_no_sid)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/", NULL, NULL);

    handle_session_delete(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_delete_success)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc", NULL, NULL);

    stub_delete_result = 1;

    handle_session_delete(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"deleted\":true"));

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_delete_fail)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc", NULL, NULL);

    stub_delete_result = -1;

    handle_session_delete(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_delete_not_found)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc", NULL, NULL);

    stub_delete_result = 0;

    handle_session_delete(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 404);

    reset_stubs(); free_req(&req);
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_session_update
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_session_update_no_sm)
{
    ServerContext ctx = make_ctx(NULL, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc", NULL, NULL);

    handle_session_update(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_update_no_sid)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/", NULL, NULL);

    handle_session_update(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_update_no_body)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc", NULL, NULL);

    handle_session_update(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_update_invalid_json)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc", "not json", NULL);

    handle_session_update(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_update_missing_title)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc", "{}", NULL);

    handle_session_update(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_update_load_fails)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc",
                                "{\"title\":\"New Title\"}", NULL);

    stub_load_result = NULL;

    handle_session_update(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 404);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_update_save_fails)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc",
                                "{\"title\":\"New Title\"}", NULL);

    Session s = {0};
    s.id = "abc";
    s.title = "Old Title";
    s.created_at = "yesterday";
    stub_load_result = &s;
    stub_save_result = -1;

    handle_session_update(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_update_success)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc",
                                "{\"title\":\"Updated Title\"}", NULL);

    Session s = {0};
    s.id = "abc";
    s.title = "Old Title";
    s.created_at = "yesterday";
    stub_load_result = &s;
    stub_save_result = 0;

    handle_session_update(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"title\":\"Updated Title\""));

    reset_stubs(); free_req(&req);
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_sessions_rename
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_sessions_rename_no_unlock)
{
    stub_unlock_result = 0;
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/rename", NULL, NULL);

    handle_sessions_rename(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 401);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_sessions_rename_no_sm)
{
    stub_unlock_result = 1;
    ServerContext ctx = make_ctx(NULL, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/rename", NULL, NULL);

    handle_sessions_rename(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_sessions_rename_no_body)
{
    stub_unlock_result = 1;
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/rename", NULL, NULL);

    handle_sessions_rename(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_sessions_rename_invalid_json)
{
    stub_unlock_result = 1;
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/rename", "bad", NULL);

    handle_sessions_rename(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_sessions_rename_missing_fields)
{
    stub_unlock_result = 1;
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/rename",
                                "{\"session_id\":\"abc\"}", NULL);

    handle_sessions_rename(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_sessions_rename_load_fails)
{
    stub_unlock_result = 1;
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/rename",
        "{\"session_id\":\"abc\",\"new_title\":\"Renamed\"}", NULL);

    stub_load_result = NULL;

    handle_sessions_rename(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 404);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_sessions_rename_success)
{
    stub_unlock_result = 1;
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/rename",
        "{\"session_id\":\"abc\",\"new_title\":\"Renamed\"}", NULL);

    Session s = {0};
    s.id = "abc";
    s.title = "Old Name";
    s.created_at = "whenever";
    stub_load_result = &s;
    stub_save_result = 0;

    handle_sessions_rename(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"title\":\"Renamed\""));

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_sessions_rename_save_fails)
{
    stub_unlock_result = 1;
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/rename",
        "{\"session_id\":\"abc\",\"new_title\":\"Renamed\"}", NULL);

    Session s = {0};
    s.id = "abc";
    s.title = "Old Name";
    s.created_at = "whenever";
    stub_load_result = &s;
    stub_save_result = -1;

    handle_sessions_rename(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    reset_stubs(); free_req(&req);
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_session_import
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_session_import_no_sm)
{
    ServerContext ctx = make_ctx(NULL, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/import", NULL, NULL);

    handle_session_import(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_import_no_body)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/import", NULL, NULL);

    handle_session_import(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_import_fails)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/import", "{\"data\":\"x\"}", NULL);

    stub_import_result_null = 1;

    handle_session_import(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs(); free_req(&req);
}
END_TEST

START_TEST(test_handle_session_import_success)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/import", "{\"data\":\"x\"}", NULL);

    stub_import_result_null = 0;

    handle_session_import(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"id\":\"imp-123\""));

    reset_stubs(); free_req(&req);
}
END_TEST

/* ---------------------------------------------------------------------------
 * Suite
 * --------------------------------------------------------------------------- */

static void setup(void) { reset_stubs(); }
static void teardown(void) { reset_stubs(); }

Suite *routes_session_handlers_suite(void)
{
    Suite *s = suite_create("routes_session_handlers");

    TCase *tc = tcase_create("handle_sessions");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_sessions_no_unlock);
    tcase_add_test(tc, test_handle_sessions_no_sm);
    tcase_add_test(tc, test_handle_sessions_list_null);
    tcase_add_test(tc, test_handle_sessions_with_data);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_create_session");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_create_session_no_unlock);
    tcase_add_test(tc, test_handle_create_session_no_sm);
    tcase_add_test(tc, test_handle_create_session_default_title);
    tcase_add_test(tc, test_handle_create_session_custom_title);
    tcase_add_test(tc, test_handle_create_session_invalid_json_still_defaults);
    tcase_add_test(tc, test_handle_create_session_fails);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_session_get");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_session_get_no_sm);
    tcase_add_test(tc, test_handle_session_get_no_sid);
    tcase_add_test(tc, test_handle_session_get_not_found);
    tcase_add_test(tc, test_handle_session_get_export);
    tcase_add_test(tc, test_handle_session_get_export_not_found);
    tcase_add_test(tc, test_handle_session_get_debug_export);
    tcase_add_test(tc, test_handle_session_get_success);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_session_delete");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_session_delete_no_sm);
    tcase_add_test(tc, test_handle_session_delete_no_sid);
    tcase_add_test(tc, test_handle_session_delete_success);
    tcase_add_test(tc, test_handle_session_delete_fail);
    tcase_add_test(tc, test_handle_session_delete_not_found);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_session_update");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_session_update_no_sm);
    tcase_add_test(tc, test_handle_session_update_no_sid);
    tcase_add_test(tc, test_handle_session_update_no_body);
    tcase_add_test(tc, test_handle_session_update_invalid_json);
    tcase_add_test(tc, test_handle_session_update_missing_title);
    tcase_add_test(tc, test_handle_session_update_load_fails);
    tcase_add_test(tc, test_handle_session_update_save_fails);
    tcase_add_test(tc, test_handle_session_update_success);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_sessions_rename");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_sessions_rename_no_unlock);
    tcase_add_test(tc, test_handle_sessions_rename_no_sm);
    tcase_add_test(tc, test_handle_sessions_rename_no_body);
    tcase_add_test(tc, test_handle_sessions_rename_invalid_json);
    tcase_add_test(tc, test_handle_sessions_rename_missing_fields);
    tcase_add_test(tc, test_handle_sessions_rename_load_fails);
    tcase_add_test(tc, test_handle_sessions_rename_success);
    tcase_add_test(tc, test_handle_sessions_rename_save_fails);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_session_import");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_session_import_no_sm);
    tcase_add_test(tc, test_handle_session_import_no_body);
    tcase_add_test(tc, test_handle_session_import_fails);
    tcase_add_test(tc, test_handle_session_import_success);
    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    int failures = 0;
    Suite *s = routes_session_handlers_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_ENV);
    failures = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failures;
}
