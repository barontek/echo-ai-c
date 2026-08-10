/*
 * test_http_client.c - Check tests for the shared libcurl response
 * buffer (http_client.c). Covers growth, NUL termination, limit
 * enforcement, size-arithmetic overflow refusal, and the curl write
 * callback's failure signaling.
 */

#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../src/utils/http_client.h"

START_TEST(test_append_grows_and_terminates)
{
    HttpBuffer b = {0};
    ck_assert_int_eq(http_buffer_append(&b, "hello", 5), 0);
    ck_assert_int_eq((int)b.len, 5);
    ck_assert_str_eq(b.data, "hello");
    ck_assert_int_eq(http_buffer_append(&b, " world", 6), 0);
    ck_assert_int_eq((int)b.len, 11);
    ck_assert_str_eq(b.data, "hello world");
    http_buffer_free(&b);
}
END_TEST

START_TEST(test_append_null_args_refused)
{
    HttpBuffer b = {0};
    ck_assert_int_eq(http_buffer_append(NULL, "x", 1), -1);
    ck_assert_int_eq(http_buffer_append(&b, NULL, 1), -1);
    ck_assert_int_eq(http_buffer_append(&b, "ok", 2), 0);
    ck_assert_str_eq(b.data, "ok");
    http_buffer_free(&b);
}
END_TEST

START_TEST(test_limit_refuses_overflow_and_sets_flag)
{
    HttpBuffer b = {.limit = 10};
    ck_assert_int_eq(http_buffer_append(&b, "0123456789", 10), 0);
    ck_assert_int_eq((int)b.len, 10);
    ck_assert_int_eq(b.too_large, 0);
    ck_assert_int_eq(http_buffer_append(&b, "X", 1), -1);
    ck_assert_int_eq(b.too_large, 1);
    ck_assert_int_eq((int)b.len, 10);          /* refused append left it untouched */
    ck_assert_str_eq(b.data, "0123456789");
    http_buffer_free(&b);
}
END_TEST

START_TEST(test_limit_refuses_single_chunk_past_limit)
{
    HttpBuffer b = {.limit = 4};
    ck_assert_int_eq(http_buffer_append(&b, "too long", 8), -1);
    ck_assert_int_eq(b.too_large, 1);
    ck_assert_int_eq((int)b.len, 0);
    ck_assert_ptr_null(b.data); /* nothing was allocated on refusal */
    http_buffer_free(&b);
}
END_TEST

START_TEST(test_arithmetic_overflow_refused_without_allocation)
{
    HttpBuffer b = {0};
    b.len = SIZE_MAX - 1U; /* forged length; no data backing it */
    ck_assert_int_eq(http_buffer_append(&b, "x", 1), -1);
    b.len = 0;
    http_buffer_free(&b);
}
END_TEST

START_TEST(test_write_cb_appends_and_signals_failure)
{
    HttpBuffer b = {0};
    ck_assert_int_eq((int)http_buffer_write_cb("abc", 1, 3, &b), 3);
    ck_assert_str_eq(b.data, "abc");
    ck_assert_int_eq((int)http_buffer_write_cb("de", 1, 2, &b), 2);
    ck_assert_str_eq(b.data, "abcde");
    http_buffer_free(&b);
}
END_TEST

START_TEST(test_write_cb_aborts_on_limit)
{
    HttpBuffer b = {.limit = 3};
    ck_assert_int_eq((int)http_buffer_write_cb("abc", 1, 3, &b), 3);
    ck_assert_int_eq((int)http_buffer_write_cb("de", 1, 2, &b), 0); /* aborts transfer */
    ck_assert_int_eq(b.too_large, 1);
    ck_assert_str_eq(b.data, "abc");
    http_buffer_free(&b);
}
END_TEST

Suite *http_client_suite(void)
{
    Suite *s = suite_create("HttpClient");
    TCase *tc_buffer = tcase_create("Buffer");
    tcase_add_test(tc_buffer, test_append_grows_and_terminates);
    tcase_add_test(tc_buffer, test_append_null_args_refused);
    tcase_add_test(tc_buffer, test_limit_refuses_overflow_and_sets_flag);
    tcase_add_test(tc_buffer, test_limit_refuses_single_chunk_past_limit);
    tcase_add_test(tc_buffer, test_arithmetic_overflow_refused_without_allocation);
    tcase_add_test(tc_buffer, test_write_cb_appends_and_signals_failure);
    tcase_add_test(tc_buffer, test_write_cb_aborts_on_limit);
    suite_add_tcase(s, tc_buffer);
    return s;
}

int main(void)
{
    int number_failed = 0;
    SRunner *sr = srunner_create(http_client_suite());
    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return number_failed == 0 ? 0 : 1;
}
