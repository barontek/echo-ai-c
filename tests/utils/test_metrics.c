#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils/metrics.h"

START_TEST(test_metrics_empty)
{
    Metrics *m = metrics_create();
    ck_assert_ptr_nonnull(m);
    char *out = metrics_render_new(m);
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
    char *out = metrics_render_new(m);
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
    char *out = metrics_render_new(m);
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
    ck_assert_int_eq(metrics_counter_inc(m, "test_total", "Test counter"), -1);
    char *out = metrics_render_new(m);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "");
    free(out);
    metrics_test_set_alloc_fail(-1);
    metrics_counter_inc(m, "other_total", "Other");
    out = metrics_render_new(m);
    ck_assert_ptr_nonnull(out);
    ck_assert_ptr_ne(strstr(out, "other_total 1"), NULL);
    free(out);
    metrics_destroy(m);
}
END_TEST

START_TEST(test_metrics_counter_full_registry_drops_with_error)
{
    Metrics *m = metrics_create();
    char name[64];
    for (int i = 0; i < 64; i++)
    {
        snprintf(name, sizeof(name), "counter_%02d_total", i);
        ck_assert_int_eq(metrics_counter_inc(m, name, NULL), 0);
    }
    ck_assert_int_eq(metrics_counter_inc(m, "overflow_total", NULL), -1);
    /* Existing series still record after the drop. */
    ck_assert_int_eq(metrics_counter_inc(m, "counter_00_total", NULL), 0);
    metrics_destroy(m);
}
END_TEST

START_TEST(test_metrics_histogram_full_registry_drops_with_error)
{
    Metrics *m = metrics_create();
    double buckets[] = {1, 5, 10};
    char name[64];
    for (int i = 0; i < 64; i++)
    {
        snprintf(name, sizeof(name), "hist_%02d", i);
        ck_assert_int_eq(metrics_histogram_observe(m, name, NULL, 2, buckets, 3), 0);
    }
    ck_assert_int_eq(metrics_histogram_observe(m, "overflow_hist", NULL, 2, buckets, 3), -1);
    ck_assert_int_eq(metrics_histogram_observe(m, "hist_00", NULL, 2, buckets, 3), 0);
    metrics_destroy(m);
}
END_TEST

START_TEST(test_metrics_histogram_alloc_fail_mid)
{
    Metrics *m = metrics_create();
    metrics_test_set_alloc_fail(3);
    double buckets[] = {1, 5, 10};
    metrics_histogram_observe(m, "test_duration", "Test", 2, buckets, 3);
    char *out = metrics_render_new(m);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "");
    free(out);
    metrics_test_set_alloc_fail(-1);
    metrics_histogram_observe(m, "test_duration2", "Test", 2, buckets, 3);
    out = metrics_render_new(m);
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
    tcase_add_test(tc_fault, test_metrics_counter_full_registry_drops_with_error);
    tcase_add_test(tc_fault, test_metrics_histogram_full_registry_drops_with_error);
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