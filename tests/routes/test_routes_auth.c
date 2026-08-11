/* test_routes_auth.c - routes auth setup/unlock/logout tests
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

START_TEST(test_handle_setup_wrong_state)
{
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    HTTPRequest req = {0};

    handle_setup(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_setup_no_body)
{
    ServerContext ctx = {0};
    ctx.state = STATE_SETUP;
    HTTPRequest req = {0};

    handle_setup(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_setup_invalid_json)
{
    ServerContext ctx = {0};
    ctx.state = STATE_SETUP;
    HTTPRequest req = {0};
    req.body = str_dup("not json");
    req.body_len = strlen(req.body);

    handle_setup(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_setup_short_password)
{
    ServerContext ctx = {0};
    ctx.state = STATE_SETUP;
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"ab\"}");
    req.body_len = strlen(req.body);

    handle_setup(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_setup_sm_create_fails)
{
    stub_sm_create_result = NULL;

    ServerContext ctx = {0};
    ctx.state = STATE_SETUP;
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"goodpassword\"}");
    req.body_len = strlen(req.body);

    handle_setup(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_setup_success)
{
    SessionManager sm = {0};
    stub_sm_create_result = &sm;

    ServerContext ctx = {0};
    ctx.state = STATE_SETUP;
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"goodpassword\"}");
    req.body_len = strlen(req.body);

    handle_setup(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(captured_body && strstr(captured_body, "\"token\":"));
    ck_assert(ctx.state == STATE_UNLOCKED);
    ck_assert_ptr_nonnull(ctx.unlock_token);
    ck_assert_uint_eq(strlen(ctx.unlock_token), 68);
    ck_assert_int_eq(strncmp(ctx.unlock_token, "tok_", 4), 0);

    free(ctx.unlock_token);
    ctx.unlock_token = NULL;
    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_wrong_state)
{
    ServerContext ctx = {0};
    ctx.state = STATE_SETUP;
    HTTPRequest req = {0};

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_rate_limited)
{
    stub_rate_allow = 0;

    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    ctx.rate_limiter = (RateLimiter *)&ctx; /* non-NULL trigger */
    HTTPRequest req = {0};

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 429);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_no_body)
{
    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_invalid_json)
{
    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};
    req.body = str_dup("bad");
    req.body_len = strlen(req.body);

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_missing_password)
{
    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};
    req.body = str_dup("{}");
    req.body_len = strlen(req.body);

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_salt_not_found)
{
    stub_salt_result = -1;

    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"test\"}");
    req.body_len = strlen(req.body);

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_key_derive_fails)
{
    stub_salt_result = 0;
    stub_key_derive_result = -1;

    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"test\"}");
    req.body_len = strlen(req.body);

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_encrypt_check_verifier_fails)
{
    stub_encrypt_check_verifier_result = -1;

    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    ctx.rate_limiter = (void *)&ctx;
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"test\"}");
    req.body_len = strlen(req.body);

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 401);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_success)
{
    SessionManager manager = {0};
    stub_sm_create_result = &manager;
    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"goodpassword\"}");
    req.body_len = strlen(req.body);

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(captured_body && strstr(captured_body, "\"token\":"));
    ck_assert(ctx.state == STATE_UNLOCKED);
    ck_assert_ptr_nonnull(ctx.unlock_token);
    ck_assert_uint_eq(strlen(ctx.unlock_token), 68);
    ck_assert_int_eq(strncmp(ctx.unlock_token, "tok_", 4), 0);

    free(ctx.unlock_token);
    ctx.unlock_token = NULL;
    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_already_unlocked_with_valid_token)
{
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = str_dup("tok_existing");
    stub_has_valid_token_result = 1;
    HTTPRequest req = {0};

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(captured_body && strstr(captured_body, "\"token\":\"tok_existing\""));
    ck_assert_int_eq(ctx.state, STATE_UNLOCKED);
    ck_assert_str_eq(ctx.unlock_token, "tok_existing");
    ck_assert_ptr_null(ctx.sm);

    free(ctx.unlock_token);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_already_unlocked_verifies_password)
{
    SessionManager manager = {0};
    stub_sm_create_result = &manager;
    stub_has_valid_token_result = 0;
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = str_dup("tok_existing");
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"goodpassword\"}");
    req.body_len = strlen(req.body);

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(captured_body && strstr(captured_body, "\"token\":\"tok_existing\""));
    /* The live session manager must not be swapped out. */
    ck_assert_int_eq(ctx.state, STATE_UNLOCKED);
    ck_assert_ptr_null(ctx.sm);
    ck_assert_str_eq(ctx.unlock_token, "tok_existing");

    free(ctx.unlock_token);
    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_already_unlocked_rejects_wrong_password)
{
    stub_sm_create_result = NULL;
    stub_encrypt_check_verifier_result = -1;
    stub_has_valid_token_result = 0;
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = str_dup("tok_existing");
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"wrong\"}");
    req.body_len = strlen(req.body);

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 401);
    ck_assert_int_eq(ctx.state, STATE_UNLOCKED);

    free(ctx.unlock_token);
    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_logout_unauthorized)
{
    stub_unlock_result = 0;

    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_logout(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 401);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_logout_success)
{
    ServerContext ctx = {0};
    ctx.unlock_token = str_dup("token123");
    ctx.state = STATE_UNLOCKED;
    HTTPRequest req = {0};

    handle_logout(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "logged out"));
    ck_assert_ptr_null(ctx.unlock_token);
    ck_assert_int_eq(ctx.state, STATE_LOCKED);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_logout_preserves_unlock_state_when_detach_fails)
{
    ServerContext ctx = {0};
    ctx.unlock_token = str_dup("token123");
    ctx.state = STATE_UNLOCKED;
    ctx.openai_oauth = (OpenAIOAuth *)&ctx;
    HTTPRequest req = {0};
    openai_oauth_stub_attach_result = -1;

    handle_logout(&req, NULL, &ctx);

    ck_assert_int_eq(captured_status, 500);
    ck_assert_ptr_nonnull(ctx.unlock_token);
    ck_assert_int_eq(ctx.state, STATE_UNLOCKED);
    openai_oauth_stub_attach_result = 0;
    free(ctx.unlock_token);
    reset_stubs();
}
END_TEST


Suite *routes_auth_auth_suite(void)
{
    Suite *s = suite_create("routes_auth_auth");
    TCase *tc = tcase_create("handle_setup");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_setup_wrong_state);
    tcase_add_test(tc, test_handle_setup_no_body);
    tcase_add_test(tc, test_handle_setup_invalid_json);
    tcase_add_test(tc, test_handle_setup_short_password);
    tcase_add_test(tc, test_handle_setup_sm_create_fails);
    tcase_add_test(tc, test_handle_setup_success);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_unlock");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_unlock_wrong_state);
    tcase_add_test(tc, test_handle_unlock_rate_limited);
    tcase_add_test(tc, test_handle_unlock_no_body);
    tcase_add_test(tc, test_handle_unlock_invalid_json);
    tcase_add_test(tc, test_handle_unlock_missing_password);
    tcase_add_test(tc, test_handle_unlock_salt_not_found);
    tcase_add_test(tc, test_handle_unlock_key_derive_fails);
    tcase_add_test(tc, test_handle_unlock_encrypt_check_verifier_fails);
    tcase_add_test(tc, test_handle_unlock_success);
    tcase_add_test(tc, test_handle_unlock_already_unlocked_with_valid_token);
    tcase_add_test(tc, test_handle_unlock_already_unlocked_verifies_password);
    tcase_add_test(tc, test_handle_unlock_already_unlocked_rejects_wrong_password);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_logout");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_logout_unauthorized);
    tcase_add_test(tc, test_handle_logout_success);
    tcase_add_test(tc, test_handle_logout_preserves_unlock_state_when_detach_fails);
    suite_add_tcase(s, tc);


    return s;
}

int main(void)
{
    int failures = 0;
    Suite *s = routes_auth_auth_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_ENV);
    failures = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failures;
}
