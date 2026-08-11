/* test_routes_auth_password.c - routes auth change-password tests
 * Split from test_routes_auth.c (2026-08 file-length compliance);
 * shared stubs live in test_routes_auth_fixture.c. Depends on:
 * check, routes_auth.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "test_routes_auth_fixture.h"

START_TEST(test_handle_change_password_no_sm)
{
    ServerContext ctx = {0};
    ctx.sm = NULL;
    HTTPRequest req = {0};

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_no_body)
{
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_invalid_json)
{
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("bad");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_short)
{
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("{\"current_password\":\"oldpass1\",\"new_password\":\"x\"}");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);
    ck_assert_ptr_nonnull(strstr(captured_body, "8 characters"));
    ck_assert_int_eq(stub_migration_call_count, 0);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_migration_fails)
{
    SessionManager probe = {0};
    stub_migration_change_result = -1;
    stub_sm_create_result = &probe;
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("{\"current_password\":\"oldpass1\",\"new_password\":\"goodpass1\"}");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);
    ck_assert_int_eq(stub_migration_call_count, 1);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_reports_committed_recovery_state)
{
    SessionManager probe = {0};
    stub_migration_change_result = -2;
    stub_sm_create_result = &probe;
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("{\"current_password\":\"oldpass1\",\"new_password\":\"goodpass1\"}");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);
    ck_assert_ptr_nonnull(strstr(captured_body, "restart"));
    ck_assert_ptr_nonnull(strstr(captured_body, "new password"));
    ck_assert_int_eq(stub_migration_call_count, 1);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_success)
{
    SessionManager probe = {0};
    stub_sm_create_result = &probe;
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("{\"current_password\":\"oldpass1\",\"new_password\":\"goodpass1\",\"confirm\":\"goodpass1\"}");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"changed\":true"));
    ck_assert_int_eq(stub_migration_call_count, 1);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_missing_current)
{
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("{\"new_password\":\"goodpass1\",\"confirm\":\"goodpass1\"}");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);
    ck_assert_ptr_nonnull(strstr(captured_body, "current password is required"));
    ck_assert_int_eq(stub_migration_call_count, 0);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_confirm_mismatch)
{
    SessionManager probe = {0};
    stub_sm_create_result = &probe;
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("{\"current_password\":\"oldpass1\",\"new_password\":\"goodpass1\",\"confirm\":\"different1\"}");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);
    ck_assert_ptr_nonnull(strstr(captured_body, "do not match"));
    ck_assert_int_eq(stub_migration_call_count, 0);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_wrong_current_rejected)
{
    /* Wrong password: verification fails before any key material changes. */
    stub_encrypt_check_verifier_result = -1;
    ServerContext ctx = {0};
    ctx.rate_limiter = (void *)&ctx;
    SessionManager sm = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("{\"current_password\":\"wrongpass\",\"new_password\":\"goodpass1\",\"confirm\":\"goodpass1\"}");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 401);
    ck_assert_ptr_nonnull(strstr(captured_body, "current password is incorrect"));
    ck_assert_int_eq(stub_migration_call_count, 0);
    ck_assert_int_eq(stub_rate_failure_count, 1);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_rate_limited)
{
    stub_rate_allow = 0;
    ServerContext ctx = {0};
    ctx.rate_limiter = (void *)&ctx;
    SessionManager sm = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("{\"current_password\":\"oldpass1\",\"new_password\":\"goodpass1\"}");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 429);
    ck_assert_int_eq(stub_migration_call_count, 0);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_storage_unavailable)
{
    /* create_ex fails for a non-auth reason: report 500, not 401. */
    stub_sm_create_result = NULL;
    stub_encrypt_check_verifier_result = 0;
    ServerContext ctx = {0};
    SessionManager sm = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("{\"current_password\":\"oldpass1\",\"new_password\":\"goodpass1\"}");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);
    ck_assert_ptr_nonnull(strstr(captured_body, "session storage unavailable"));
    ck_assert_int_eq(stub_migration_call_count, 0);

    free(req.body);
    reset_stubs();
}
END_TEST


Suite *routes_auth_password_suite(void)
{
    Suite *s = suite_create("routes_auth_password");
    TCase *tc = tcase_create("handle_change_password");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_change_password_no_sm);
    tcase_add_test(tc, test_handle_change_password_no_body);
    tcase_add_test(tc, test_handle_change_password_invalid_json);
    tcase_add_test(tc, test_handle_change_password_short);
    tcase_add_test(tc, test_handle_change_password_migration_fails);
    tcase_add_test(tc, test_handle_change_password_reports_committed_recovery_state);
    tcase_add_test(tc, test_handle_change_password_success);
    tcase_add_test(tc, test_handle_change_password_missing_current);
    tcase_add_test(tc, test_handle_change_password_confirm_mismatch);
    tcase_add_test(tc, test_handle_change_password_wrong_current_rejected);
    tcase_add_test(tc, test_handle_change_password_rate_limited);
    tcase_add_test(tc, test_handle_change_password_storage_unavailable);
    suite_add_tcase(s, tc);


    return s;
}

int main(void)
{
    int failures = 0;
    Suite *s = routes_auth_password_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_ENV);
    failures = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failures;
}
