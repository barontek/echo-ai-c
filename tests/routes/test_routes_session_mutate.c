/* test_routes_session_mutate.c - session route handle_session_delete, handle_session_update, handle_sessions_rename tests
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


Suite *routes_session_mutate_suite(void)
{
    Suite *s = suite_create("routes_session_mutate");
    TCase *tc = tcase_create("handle_session_delete");
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


    return s;
}

int main(void)
{
    int failures = 0;
    Suite *s = routes_session_mutate_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_ENV);
    failures = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failures;
}
