/* test_route_table.c - gating contract of the real HTTP route table.
 * Compiles routes.c WITHOUT ROUTES_TEST so the actual routes[]/routes_count
 * data is under test (handler modules are stubbed here). Depends on:
 * check, routes.c.
 */

#define _GNU_SOURCE
#include <check.h>
#include <string.h>

#include "../src/server/routes/routes.h"

/* ---------------------------------------------------------------------------
 * Handler stubs: the table references these symbols; this binary only
 * asserts table data, so the bodies are intentionally empty.
 * --------------------------------------------------------------------------- */

void handle_setup(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_unlock(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_logout(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_change_password(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_openai_oauth_status(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_openai_oauth_start(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_openai_oauth_logout(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_sessions(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_create_session(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_session_get(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_session_delete(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_session_update(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_sessions_rename(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_session_import(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_chat(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_sse_stream(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_status(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_health(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_config(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_metrics(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_undo(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_redo(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_health_detailed(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_models(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }
void handle_providers(HTTPRequest *req, Client *client, ServerContext *ctx)
{ (void)req; (void)client; (void)ctx; }

/* ---------------------------------------------------------------------------
 * Table lookups
 * --------------------------------------------------------------------------- */

static const Route *find_route(const char *method, const char *path)
{
    for (int i = 0; i < routes_count; i++)
    {
        if (strcmp(routes[i].method, method) == 0 &&
            strcmp(routes[i].path, path) == 0)
            return &routes[i];
    }
    return NULL;
}

START_TEST(test_health_detailed_requires_unlock)
{
    /* Regression: GET /api/health/detailed used to be registered ungated
     * (0,0,0), which let unauthenticated callers drive title decryption
     * on every request — the timing oracle for the HMAC compare plus an
     * unauthenticated CPU trigger. It must be gated like every other
     * session-touching route. */
    const Route *r = find_route("GET", "/api/health/detailed");
    ck_assert_ptr_nonnull(r);
    ck_assert_int_eq(r->is_prefix, 0);
    ck_assert_int_eq(r->needs_unlock, 1);
    ck_assert_int_eq(r->unlock_via_query, 0);
}
END_TEST

START_TEST(test_session_routes_all_require_unlock)
{
    const char *const session_paths[] = {
        "/api/sessions", "/api/sessions/rename", "/api/sessions/import",
        "/api/sessions/", "/api/chat", "/api/stream",
        "/api/change-password", "/api/logout",
        "/api/auth/openai/status", "/api/auth/openai/start",
        "/api/auth/openai/logout", "/api/undo", "/api/redo"
    };
    for (size_t i = 0; i < sizeof(session_paths) / sizeof(session_paths[0]); i++)
    {
        const Route *r = find_route("GET", session_paths[i]);
        if (!r) r = find_route("POST", session_paths[i]);
        if (!r) r = find_route("PUT", session_paths[i]);
        if (!r) r = find_route("DELETE", session_paths[i]);
        ck_assert_ptr_nonnull(r);
        ck_assert_int_eq(r->needs_unlock, 1);
    }
}
END_TEST

START_TEST(test_public_routes_stay_ungated)
{
    const char *const public_paths[] = {
        "/api/status", "/api/health", "/api/config",
        "/api/setup", "/api/unlock", "/api/models",
        "/api/providers", "/api/metrics"
    };
    for (size_t i = 0; i < sizeof(public_paths) / sizeof(public_paths[0]); i++)
    {
        const Route *r = find_route("GET", public_paths[i]);
        if (!r) r = find_route("POST", public_paths[i]);
        ck_assert_ptr_nonnull(r);
        ck_assert_int_eq(r->needs_unlock, 0);
    }
}
END_TEST

Suite *route_table_suite(void)
{
    Suite *s = suite_create("RouteTable");

    TCase *tc = tcase_create("gating");
    tcase_add_test(tc, test_health_detailed_requires_unlock);
    tcase_add_test(tc, test_session_routes_all_require_unlock);
    tcase_add_test(tc, test_public_routes_stay_ungated);
    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    Suite *s = route_table_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
