/* test_routes_session_import.c - session route handle_session_import tests
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


Suite *routes_session_import_suite(void)
{
    Suite *s = suite_create("routes_session_import");
    TCase *tc = tcase_create("handle_session_import");
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
    Suite *s = routes_session_import_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_ENV);
    failures = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failures;
}
