#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "utils/circuit_breaker.h"

START_TEST(test_cb_starts_closed)
{
    CircuitBreaker *cb = cb_create(5, 30000);
    ck_assert_ptr_nonnull(cb);
    ck_assert_int_eq(cb->state, CB_CLOSED);
    ck_assert_int_eq(cb->failure_threshold, 5);
    ck_assert_int_eq(cb->half_open_timeout_ms, 30000);
    cb_destroy(cb);
}
END_TEST

START_TEST(test_cb_opens_after_threshold)
{
    CircuitBreaker *cb = cb_create(3, 30000);
    ck_assert_int_eq(cb_is_available(cb), 1);
    cb_record_failure(cb);
    ck_assert_int_eq(cb_is_available(cb), 1);
    cb_record_failure(cb);
    ck_assert_int_eq(cb_is_available(cb), 1);
    cb_record_failure(cb);
    ck_assert_int_eq(cb->state, CB_OPEN);
    ck_assert_int_eq(cb_is_available(cb), 0);
    cb_destroy(cb);
}
END_TEST

START_TEST(test_cb_success_resets)
{
    CircuitBreaker *cb = cb_create(2, 30000);
    cb_record_failure(cb);
    cb_record_success(cb);
    ck_assert_int_eq(cb->state, CB_CLOSED);
    ck_assert_int_eq(cb->failure_count, 0);
    cb_destroy(cb);
}
END_TEST

START_TEST(test_cb_half_open_transitions)
{
    CircuitBreaker *cb = cb_create(1, 1);
    cb_record_failure(cb);
    ck_assert_int_eq(cb->state, CB_OPEN);
    ck_assert_int_eq(cb_is_available(cb), 0);

    while (cb_now_ms() - cb->opened_at_ms < 2) { }

    ck_assert_int_eq(cb_is_available(cb), 1);
    ck_assert_int_eq(cb->state, CB_HALF_OPEN);

    cb_record_success(cb);
    ck_assert_int_eq(cb->state, CB_CLOSED);
    cb_destroy(cb);
}
END_TEST

START_TEST(test_cb_null_safe)
{
    ck_assert_int_eq(cb_is_available(NULL), 1);
    cb_record_success(NULL);
    cb_record_failure(NULL);
    cb_destroy(NULL);
}
END_TEST

Suite *circuit_breaker_suite(void)
{
    Suite *s = suite_create("CircuitBreaker");
    TCase *tc = tcase_create("Core");
    tcase_add_test(tc, test_cb_starts_closed);
    tcase_add_test(tc, test_cb_opens_after_threshold);
    tcase_add_test(tc, test_cb_success_resets);
    tcase_add_test(tc, test_cb_half_open_transitions);
    tcase_add_test(tc, test_cb_null_safe);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = circuit_breaker_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
