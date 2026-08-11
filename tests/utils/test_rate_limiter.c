#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "utils/rate_limiter.h"

/* test_rate_limiter - unit tests for rate limiter. Depends on: check, the module under test. */
START_TEST(test_rl_create_destroy_roundtrip)
{
    RateLimiter *rl = rate_limiter_create(10, 60, NULL);
    ck_assert_ptr_nonnull(rl);
    rate_limiter_destroy(rl);
}
END_TEST

START_TEST(test_rl_destroy_null)
{
    rate_limiter_destroy(NULL);
}
END_TEST

START_TEST(test_rl_allow_null_params)
{
    ck_assert_int_eq(rate_limiter_allow(NULL, "127.0.0.1"), 1);
    RateLimiter *rl = rate_limiter_create(10, 60, NULL);
    ck_assert_int_eq(rate_limiter_allow(rl, NULL), 1);
    rate_limiter_destroy(rl);
}
END_TEST

START_TEST(test_rl_allow_new_ip)
{
    RateLimiter *rl = rate_limiter_create(10, 60, NULL);
    ck_assert_int_eq(rate_limiter_allow(rl, "192.168.1.1"), 1);
    rate_limiter_destroy(rl);
}
END_TEST

START_TEST(test_rl_allow_multiple_within_window)
{
    RateLimiter *rl = rate_limiter_create(3, 60, NULL);
    ck_assert_int_eq(rate_limiter_allow(rl, "10.0.0.1"), 1);
    ck_assert_int_eq(rate_limiter_allow(rl, "10.0.0.1"), 1);
    ck_assert_int_eq(rate_limiter_allow(rl, "10.0.0.1"), 1);
    rate_limiter_destroy(rl);
}
END_TEST

START_TEST(test_rl_allow_exceeds_limit)
{
    RateLimiter *rl = rate_limiter_create(2, 60, NULL);
    ck_assert_int_eq(rate_limiter_allow(rl, "10.0.0.2"), 1);
    ck_assert_int_eq(rate_limiter_allow(rl, "10.0.0.2"), 1);
    ck_assert_int_eq(rate_limiter_allow(rl, "10.0.0.2"), 0);
    rate_limiter_destroy(rl);
}
END_TEST

START_TEST(test_rl_allow_different_ips_independent)
{
    RateLimiter *rl = rate_limiter_create(1, 60, NULL);
    ck_assert_int_eq(rate_limiter_allow(rl, "10.0.0.3"), 1);
    ck_assert_int_eq(rate_limiter_allow(rl, "10.0.0.4"), 1);
    rate_limiter_destroy(rl);
}
END_TEST

START_TEST(test_rl_allow_window_expired)
{
    RateLimiter *rl = rate_limiter_create(1, 0, NULL);
    ck_assert_int_eq(rate_limiter_allow(rl, "10.0.0.5"), 1);
    ck_assert_int_eq(rate_limiter_allow(rl, "10.0.0.5"), 1);
    rate_limiter_destroy(rl);
}
END_TEST

START_TEST(test_rl_record_unlock_failure)
{
    RateLimiter *rl = rate_limiter_create(10, 60, NULL);
    rate_limiter_record_unlock_failure(rl, "192.168.1.1");
    rate_limiter_record_unlock_failure(rl, "192.168.1.1");
    rate_limiter_record_unlock_failure(rl, NULL);
    rate_limiter_record_unlock_failure(NULL, "192.168.1.1");
    rate_limiter_destroy(rl);
}
END_TEST

START_TEST(test_rl_unlock_allowed_within_limits)
{
    RateLimiter *rl = rate_limiter_create(10, 60, NULL);
    ck_assert_int_eq(rate_limiter_unlock_allowed(rl, "10.0.0.6", 3, 10), 1);
    rate_limiter_destroy(rl);
}
END_TEST

START_TEST(test_rl_unlock_allowed_null_params)
{
    ck_assert_int_eq(rate_limiter_unlock_allowed(NULL, "ip", 3, 10), 1);
    RateLimiter *rl = rate_limiter_create(10, 60, NULL);
    ck_assert_int_eq(rate_limiter_unlock_allowed(rl, NULL, 3, 10), 1);
    rate_limiter_destroy(rl);
}
END_TEST

START_TEST(test_rl_unlock_allowed_per_ip_limit)
{
    RateLimiter *rl = rate_limiter_create(10, 60, NULL);
    rate_limiter_record_unlock_failure(rl, "10.0.0.7");
    rate_limiter_record_unlock_failure(rl, "10.0.0.7");
    rate_limiter_record_unlock_failure(rl, "10.0.0.7");
    ck_assert_int_eq(rate_limiter_unlock_allowed(rl, "10.0.0.7", 2, 10), 0);
    rate_limiter_destroy(rl);
}
END_TEST

START_TEST(test_rl_unlock_allowed_global_limit)
{
    RateLimiter *rl = rate_limiter_create(10, 60, NULL);
    rate_limiter_record_unlock_failure(rl, "10.0.0.8");
    rate_limiter_record_unlock_failure(rl, "10.0.0.9");
    rate_limiter_record_unlock_failure(rl, "10.0.0.10");
    ck_assert_int_eq(rate_limiter_unlock_allowed(rl, "10.0.0.11", 10, 2), 0);
    rate_limiter_destroy(rl);
}
END_TEST

START_TEST(test_rl_unlock_other_ip_not_blocked)
{
    RateLimiter *rl = rate_limiter_create(10, 60, NULL);
    rate_limiter_record_unlock_failure(rl, "10.0.0.12");
    rate_limiter_record_unlock_failure(rl, "10.0.0.12");
    ck_assert_int_eq(rate_limiter_unlock_allowed(rl, "10.0.0.13", 2, 10), 1);
    rate_limiter_destroy(rl);
}
END_TEST

Suite *rate_limiter_suite(void)
{
    Suite *s = suite_create("RateLimiter");
    TCase *tc = tcase_create("Lifecycle");
    tcase_set_timeout(tc, 10);
    tcase_add_test(tc, test_rl_create_destroy_roundtrip);
    tcase_add_test(tc, test_rl_destroy_null);
    suite_add_tcase(s, tc);

    TCase *tc_allow = tcase_create("Allow");
    tcase_set_timeout(tc_allow, 10);
    tcase_add_test(tc_allow, test_rl_allow_null_params);
    tcase_add_test(tc_allow, test_rl_allow_new_ip);
    tcase_add_test(tc_allow, test_rl_allow_multiple_within_window);
    tcase_add_test(tc_allow, test_rl_allow_exceeds_limit);
    tcase_add_test(tc_allow, test_rl_allow_different_ips_independent);
    tcase_add_test(tc_allow, test_rl_allow_window_expired);
    suite_add_tcase(s, tc_allow);

    TCase *tc_unlock = tcase_create("Unlock");
    tcase_set_timeout(tc_unlock, 10);
    tcase_add_test(tc_unlock, test_rl_record_unlock_failure);
    tcase_add_test(tc_unlock, test_rl_unlock_allowed_within_limits);
    tcase_add_test(tc_unlock, test_rl_unlock_allowed_null_params);
    tcase_add_test(tc_unlock, test_rl_unlock_allowed_per_ip_limit);
    tcase_add_test(tc_unlock, test_rl_unlock_allowed_global_limit);
    tcase_add_test(tc_unlock, test_rl_unlock_other_ip_not_blocked);
    suite_add_tcase(s, tc_unlock);

    return s;
}

int main(void)
{
    Suite *s = rate_limiter_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
