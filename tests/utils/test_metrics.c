#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "utils/metrics.h"

START_TEST(test_metrics_empty)
{
    Metrics *m = metrics_create();
    ck_assert_ptr_nonnull(m);
    char *out = metrics_render(m);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "");
    free(out);
    metrics_destroy(m);
}
END_TEST

START_TEST(test_metrics_counter_increments_and_renders_prometheus_format)
{
    Metrics *m = metrics_create();
    metrics_counter_inc(m, "test_total", "Test counter");
    metrics_counter_inc(m, "test_total", "");
    metrics_counter_inc(m, "other_total", "Other");
    char *out = metrics_render(m);
    ck_assert_ptr_nonnull(out);
    ck_assert_ptr_ne(strstr(out, "test_total 2"), NULL);
    ck_assert_ptr_ne(strstr(out, "other_total 1"), NULL);
    free(out);
    metrics_destroy(m);
}
END_TEST

START_TEST(test_metrics_histogram_observes_and_renders_buckets_and_sum)
{
    Metrics *m = metrics_create();
    double buckets[] = {1, 5, 10};
    metrics_histogram_observe(m, "test_duration", "Test", 2, buckets, 3);
    metrics_histogram_observe(m, "test_duration", "Test", 7, buckets, 3);
    metrics_histogram_observe(m, "test_duration", "Test", 12, buckets, 3);
    char *out = metrics_render(m);
    ck_assert_ptr_nonnull(out);
    ck_assert_ptr_ne(strstr(out, "test_duration_count 3"), NULL);
    ck_assert_ptr_ne(strstr(out, "test_duration_sum 21"), NULL);
    ck_assert_ptr_ne(strstr(out, "_bucket{le=\"1\"} 0"), NULL);
    ck_assert_ptr_ne(strstr(out, "_bucket{le=\"5\"} 1"), NULL);
    ck_assert_ptr_ne(strstr(out, "_bucket{le=\"10\"} 1"), NULL);
    free(out);
    metrics_destroy(m);
}
END_TEST

START_TEST(test_metrics_counter_alloc_fail_mid)
{
    Metrics *m = metrics_create();
    metrics_test_set_alloc_fail(2);
    metrics_counter_inc(m, "test_total", "Test counter");
    char *out = metrics_render(m);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "");
    free(out);
    metrics_test_set_alloc_fail(-1);
    metrics_counter_inc(m, "other_total", "Other");
    out = metrics_render(m);
    ck_assert_ptr_nonnull(out);
    ck_assert_ptr_ne(strstr(out, "other_total 1"), NULL);
    free(out);
    metrics_destroy(m);
}
END_TEST

START_TEST(test_metrics_histogram_alloc_fail_mid)
{
    Metrics *m = metrics_create();
    metrics_test_set_alloc_fail(3);
    double buckets[] = {1, 5, 10};
    metrics_histogram_observe(m, "test_duration", "Test", 2, buckets, 3);
    char *out = metrics_render(m);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "");
    free(out);
    metrics_test_set_alloc_fail(-1);
    metrics_histogram_observe(m, "test_duration2", "Test", 2, buckets, 3);
    out = metrics_render(m);
    ck_assert_ptr_nonnull(out);
    ck_assert_ptr_ne(strstr(out, "test_duration2_count 1"), NULL);
    free(out);
    metrics_destroy(m);
}
END_TEST

Suite *metrics_suite(void)
{
    Suite *s = suite_create("Metrics");

    TCase *tc_render = tcase_create("Rendering");
    tcase_add_test(tc_render, test_metrics_empty);
    tcase_add_test(tc_render, test_metrics_counter_increments_and_renders_prometheus_format);
    tcase_add_test(tc_render, test_metrics_histogram_observes_and_renders_buckets_and_sum);
    suite_add_tcase(s, tc_render);

    TCase *tc_fault = tcase_create("FaultInjection");
    tcase_add_test(tc_fault, test_metrics_counter_alloc_fail_mid);
    tcase_add_test(tc_fault, test_metrics_histogram_alloc_fail_mid);
    suite_add_tcase(s, tc_fault);

    return s;
}

int main(void)
{
    Suite *s = metrics_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}