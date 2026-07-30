#include <check.h>
#include <string.h>

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

    suite_add_tcase(s, tc);
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
