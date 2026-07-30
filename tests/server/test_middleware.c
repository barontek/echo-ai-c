#include <check.h>
#include <string.h>
#include <stdlib.h>

#include "server/middleware.h"

int rate_limiter_allow(RateLimiter *rl, const char *ip)
{
    (void)rl;
    (void)ip;
    return 1;
}

void server_response(Client *client, int status, const char *content_type,
                     const char *body)
{
    (void)client;
    (void)status;
    (void)content_type;
    (void)body;
}

void log_error(const char *fmt, ...) { (void)fmt; }
void log_init(void) {}
void log_cleanup(void) {}
void log_set_level(int l) { (void)l; }
void log_msg(int l, const char *f, int line, const char *fmt, ...)
{ (void)l; (void)f; (void)line; (void)fmt; }

START_TEST(test_token_exact_match_returns_valid)
{
    const char *headers = "Host: localhost\r\n"
                          "X-Unlock-Token: secret123\r\n"
                          "Content-Type: application/json\r\n";
    int result = middleware_has_valid_token(headers, "secret123");
    ck_assert_int_eq(result, 1);
}
END_TEST

START_TEST(test_token_with_extra_garbage_appended_returns_invalid)
{
    const char *headers = "Host: localhost\r\n"
                          "X-Unlock-Token: secret123EXTRA\r\n"
                          "Content-Type: application/json\r\n";
    int result = middleware_has_valid_token(headers, "secret123");
    ck_assert_int_eq(result, 0);
}
END_TEST

START_TEST(test_token_substring_in_longer_string_returns_invalid)
{
    const char *headers = "Host: localhost\r\n"
                          "X-Unlock-Token: NOTsecret123MORE\r\n"
                          "Content-Type: application/json\r\n";
    int result = middleware_has_valid_token(headers, "secret123");
    ck_assert_int_eq(result, 0);
}
END_TEST

START_TEST(test_token_different_case_returns_invalid)
{
    const char *headers = "Host: localhost\r\n"
                          "X-Unlock-Token: SECRET123\r\n"
                          "Content-Type: application/json\r\n";
    int result = middleware_has_valid_token(headers, "secret123");
    ck_assert_int_eq(result, 0);
}
END_TEST

START_TEST(test_empty_token_returns_invalid)
{
    const char *headers = "Host: localhost\r\n"
                          "X-Unlock-Token: \r\n"
                          "Content-Type: application/json\r\n";
    int result = middleware_has_valid_token(headers, "secret123");
    ck_assert_int_eq(result, 0);
}
END_TEST

START_TEST(test_no_unlock_header_returns_invalid)
{
    const char *headers = "Host: localhost\r\n"
                          "Content-Type: application/json\r\n";
    int result = middleware_has_valid_token(headers, "secret123");
    ck_assert_int_eq(result, 0);
}
END_TEST

START_TEST(test_empty_token_value_empty_stored_returns_valid)
{
    const char *headers = "Host: localhost\r\n"
                          "X-Unlock-Token: \r\n"
                          "Content-Type: application/json\r\n";
    int result = middleware_has_valid_token(headers, "");
    ck_assert_int_eq(result, 1);
}
END_TEST

START_TEST(test_token_ends_at_crlf)
{
    const char *headers = "Host: localhost\r\n"
                          "X-Unlock-Token: abc\r\n"
                          "Content-Type: application/json\r\n";
    int result = middleware_has_valid_token(headers, "abc");
    ck_assert_int_eq(result, 1);
}
END_TEST

START_TEST(test_token_with_single_newline_termination)
{
    const char *headers = "Host: localhost\n"
                          "X-Unlock-Token: myToken\n"
                          "Content-Type: application/json\n";
    int result = middleware_has_valid_token(headers, "myToken");
    ck_assert_int_eq(result, 1);
}
END_TEST

START_TEST(test_token_ends_at_null)
{
    const char *headers = "X-Unlock-Token: exact";
    int result = middleware_has_valid_token(headers, "exact");
    ck_assert_int_eq(result, 1);
}
END_TEST

START_TEST(test_null_headers_returns_invalid)
{
    int result = middleware_has_valid_token(NULL, "secret");
    ck_assert_int_eq(result, 0);
}
END_TEST

START_TEST(test_null_token_returns_invalid)
{
    const char *headers = "X-Unlock-Token: secret\r\n";
    int result = middleware_has_valid_token(headers, NULL);
    ck_assert_int_eq(result, 0);
}
END_TEST

START_TEST(test_both_null_returns_invalid)
{
    int result = middleware_has_valid_token(NULL, NULL);
    ck_assert_int_eq(result, 0);
}
END_TEST

START_TEST(test_token_header_case_insensitive)
{
    const char *headers = "Host: localhost\r\n"
                          "x-unlock-token: secret\r\n"
                          "Content-Type: application/json\r\n";
    int result = middleware_has_valid_token(headers, "secret");
    ck_assert_int_eq(result, 1);
}
END_TEST

START_TEST(test_token_truncated_shorter_than_stored_returns_invalid)
{
    const char *headers = "Host: localhost\r\n"
                          "X-Unlock-Token: sec\r\n"
                          "Content-Type: application/json\r\n";
    int result = middleware_has_valid_token(headers, "secret");
    ck_assert_int_eq(result, 0);
}
END_TEST

START_TEST(test_token_with_leading_spaces)
{
    const char *headers = "Host: localhost\r\n"
                          "X-Unlock-Token:    secret\r\n"
                          "Content-Type: application/json\r\n";
    int result = middleware_has_valid_token(headers, "secret");
    ck_assert_int_eq(result, 1);
}
END_TEST

START_TEST(test_token_exact_prefix_but_shorter_returns_invalid)
{
    const char *headers = "Host: localhost\r\n"
                          "X-Unlock-Token: sec\r\n"
                          "Content-Type: application/json\r\n";
    int result = middleware_has_valid_token(headers, "secret123");
    ck_assert_int_eq(result, 0);
}
END_TEST

START_TEST(test_websocket_protocol_accepts_exact_token)
{
    const char *headers =
        "Sec-WebSocket-Protocol: echo-ai, echo-ai-token-secret\r\n";
    ck_assert_int_eq(middleware_has_valid_ws_token(headers, "secret"), 1);
}
END_TEST

START_TEST(test_websocket_protocol_rejects_token_suffix)
{
    const char *headers =
        "Sec-WebSocket-Protocol: echo-ai, echo-ai-token-secret-extra\r\n";
    ck_assert_int_eq(middleware_has_valid_ws_token(headers, "secret"), 0);
}
END_TEST

START_TEST(test_token_equals_exact)
{
    ck_assert_int_eq(token_equals("abc", 3, "abc"), 1);
}
END_TEST

START_TEST(test_token_equals_wrong_length)
{
    ck_assert_int_eq(token_equals("ab", 2, "abc"), 0);
}
END_TEST

START_TEST(test_token_equals_wrong_content)
{
    ck_assert_int_eq(token_equals("abd", 3, "abc"), 0);
}
END_TEST

START_TEST(test_token_equals_null_a)
{
    ck_assert_int_eq(token_equals(NULL, 3, "abc"), 0);
}
END_TEST

START_TEST(test_token_equals_null_b)
{
    ck_assert_int_eq(token_equals("abc", 3, NULL), 0);
}
END_TEST

START_TEST(test_token_equals_both_null)
{
    ck_assert_int_eq(token_equals(NULL, 0, NULL), 0);
}
END_TEST

START_TEST(test_token_equals_empty)
{
    ck_assert_int_eq(token_equals("", 0, ""), 1);
}
END_TEST

START_TEST(test_token_equals_empty_vs_nonempty)
{
    ck_assert_int_eq(token_equals("", 0, "a"), 0);
}
END_TEST

START_TEST(test_check_unlock_query_no_token_param)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = "secret";
    memcpy(req.query, "foo=bar", 8);
    ck_assert_int_eq(middleware_check_unlock_query(&req, &ctx), 0);
}
END_TEST

START_TEST(test_check_unlock_query_empty_query)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = "secret";
    ck_assert_int_eq(middleware_check_unlock_query(&req, &ctx), 0);
}
END_TEST

START_TEST(test_check_unlock_query_correct_token)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = "s3cret";
    memcpy(req.query, "token=s3cret", 13);
    ck_assert_int_eq(middleware_check_unlock_query(&req, &ctx), 1);
}
END_TEST

START_TEST(test_check_unlock_query_wrong_token)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = "correct";
    memcpy(req.query, "token=wrong", 12);
    ck_assert_int_eq(middleware_check_unlock_query(&req, &ctx), 0);
}
END_TEST

START_TEST(test_check_unlock_query_token_with_ampersand)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = "tok";
    memcpy(req.query, "token=tok&next=val", 19);
    ck_assert_int_eq(middleware_check_unlock_query(&req, &ctx), 1);
}
END_TEST

START_TEST(test_check_unlock_query_empty_token_value)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = "secret";
    memcpy(req.query, "token=", 7);
    ck_assert_int_eq(middleware_check_unlock_query(&req, &ctx), 0);
}
END_TEST

START_TEST(test_check_unlock_query_locked_state)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    ctx.unlock_token = "secret";
    memcpy(req.query, "token=secret", 13);
    ck_assert_int_eq(middleware_check_unlock_query(&req, &ctx), 0);
}
END_TEST

START_TEST(test_check_unlock_query_null_unlock_token)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = NULL;
    memcpy(req.query, "token=x", 8);
    ck_assert_int_eq(middleware_check_unlock_query(&req, &ctx), 0);
}
END_TEST

START_TEST(test_check_unlock_query_null_req)
{
    ServerContext ctx = {0};
    ck_assert_int_eq(middleware_check_unlock_query(NULL, &ctx), 0);
}
END_TEST

Suite *middleware_suite(void)
{
    Suite *s = suite_create("middleware");
    TCase *tc = tcase_create("has_valid_token");

    tcase_add_test(tc, test_token_exact_match_returns_valid);
    tcase_add_test(tc, test_token_with_extra_garbage_appended_returns_invalid);
    tcase_add_test(tc, test_token_substring_in_longer_string_returns_invalid);
    tcase_add_test(tc, test_token_different_case_returns_invalid);
    tcase_add_test(tc, test_empty_token_returns_invalid);
    tcase_add_test(tc, test_no_unlock_header_returns_invalid);
    tcase_add_test(tc, test_empty_token_value_empty_stored_returns_valid);
    tcase_add_test(tc, test_token_ends_at_crlf);
    tcase_add_test(tc, test_token_with_single_newline_termination);
    tcase_add_test(tc, test_token_ends_at_null);
    tcase_add_test(tc, test_null_headers_returns_invalid);
    tcase_add_test(tc, test_null_token_returns_invalid);
    tcase_add_test(tc, test_both_null_returns_invalid);
    tcase_add_test(tc, test_token_header_case_insensitive);
    tcase_add_test(tc, test_token_truncated_shorter_than_stored_returns_invalid);
    tcase_add_test(tc, test_token_with_leading_spaces);
    tcase_add_test(tc, test_token_exact_prefix_but_shorter_returns_invalid);
    tcase_add_test(tc, test_websocket_protocol_accepts_exact_token);
    tcase_add_test(tc, test_websocket_protocol_rejects_token_suffix);

    suite_add_tcase(s, tc);

    TCase *tc_te = tcase_create("token_equals");
    tcase_add_test(tc_te, test_token_equals_exact);
    tcase_add_test(tc_te, test_token_equals_wrong_length);
    tcase_add_test(tc_te, test_token_equals_wrong_content);
    tcase_add_test(tc_te, test_token_equals_null_a);
    tcase_add_test(tc_te, test_token_equals_null_b);
    tcase_add_test(tc_te, test_token_equals_both_null);
    tcase_add_test(tc_te, test_token_equals_empty);
    tcase_add_test(tc_te, test_token_equals_empty_vs_nonempty);
    suite_add_tcase(s, tc_te);

    TCase *tc_uq = tcase_create("check_unlock_query");
    tcase_add_test(tc_uq, test_check_unlock_query_no_token_param);
    tcase_add_test(tc_uq, test_check_unlock_query_empty_query);
    tcase_add_test(tc_uq, test_check_unlock_query_correct_token);
    tcase_add_test(tc_uq, test_check_unlock_query_wrong_token);
    tcase_add_test(tc_uq, test_check_unlock_query_token_with_ampersand);
    tcase_add_test(tc_uq, test_check_unlock_query_empty_token_value);
    tcase_add_test(tc_uq, test_check_unlock_query_locked_state);
    tcase_add_test(tc_uq, test_check_unlock_query_null_unlock_token);
    tcase_add_test(tc_uq, test_check_unlock_query_null_req);
    suite_add_tcase(s, tc_uq);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s = middleware_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_VERBOSE);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}
