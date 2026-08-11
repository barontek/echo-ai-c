/* test_routes_session_get.c - session route handle_session_get tests
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

START_TEST(test_handle_session_get_includes_branches)
{
    SessionManager sm = {0};
    ServerContext ctx = make_ctx(&sm, STATE_UNLOCKED, NULL);
    HTTPRequest req = make_req("/api/sessions/abc-123", NULL, NULL);

    Session s = {0};
    s.id = "abc-123";
    s.title = "Chat Session";
    s.created_at = "2024-01-01";
    stub_load_result = &s;
    stub_branch_info_json =
        "[{\"message_id\":\"m1\",\"count\":2,\"active\":1}]";

    handle_session_get(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"branches\":"));
    ck_assert(strstr(captured_body, "\"message_id\":\"m1\""));
    ck_assert(strstr(captured_body, "\"active\":1"));

    reset_stubs(); free_req(&req);
    stub_branch_info_json = NULL;
}


Suite *routes_session_get_suite(void)
{
    Suite *s = suite_create("routes_session_get");
    TCase *tc = tcase_create("handle_session_get");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_session_get_no_sm);
    tcase_add_test(tc, test_handle_session_get_no_sid);
    tcase_add_test(tc, test_handle_session_get_not_found);
    tcase_add_test(tc, test_handle_session_get_export);
    tcase_add_test(tc, test_handle_session_get_export_not_found);
    tcase_add_test(tc, test_handle_session_get_debug_export);
    tcase_add_test(tc, test_handle_session_get_success);
    tcase_add_test(tc, test_handle_session_get_includes_branches);
    suite_add_tcase(s, tc);


    return s;
}

int main(void)
{
    int failures = 0;
    Suite *s = routes_session_get_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_ENV);
    failures = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failures;
}
