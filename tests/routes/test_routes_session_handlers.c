/* test_routes_session_handlers.c - session route handle_sessions, handle_create_session tests
 * Split from test_routes_session_handlers.c (2026-08 file-length
 * compliance); shared stubs and fixtures live in
 * test_routes_session_fixture.c. Depends on: check, routes_session.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "test_routes_session_fixture.h"

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
    /* C8: a failing session store must surface as an error, not as an
     * empty session list (a DB outage looked identical to "no sessions").
     * The old contract returned 200 {"sessions":[]} here. */
    stub_unlock_result = 1;
    stub_list_result_null = 1;
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions", NULL, NULL);

    handle_sessions(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);
    ck_assert(captured_body && strstr(captured_body, "session store error"));

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

/* E2: the routes_session alloc-fail hook was dead — wired into the build
 * but never invoked. Fail the default-title str_dup (1) and the
 * body-title str_dup (2); each must abort with a 500 and never commit a
 * session. */
START_TEST(test_handle_create_session_title_alloc_fail_returns_500)
{
    stub_unlock_result = 1;
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);

    for (int fail_at = 1; fail_at <= 2; fail_at++)
    {
        HTTPRequest req = make_req("/api/sessions",
                                   fail_at == 2
                                       ? "{\"title\":\"custom\"}"
                                       : NULL,
                                   NULL);
        routes_session_test_set_alloc_fail(fail_at);
        handle_create_session(&req, NULL, &ctx);
        routes_session_test_set_alloc_fail(-1);
        /* the failed title dup aborts before any session is created */
        ck_assert_int_eq(captured_status, 500);
        free_req(&req);
        reset_stubs();
    }
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
    tcase_add_test(tc, test_handle_create_session_title_alloc_fail_returns_500);
    tcase_add_test(tc, test_handle_create_session_invalid_json_still_defaults);
    tcase_add_test(tc, test_handle_create_session_fails);
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
