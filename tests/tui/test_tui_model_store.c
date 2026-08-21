/*
 * test_tui_model_store.c - recent/favorite model lists: load/save
 * round-trip, record-recent front-insert + dedup + cap, favorite toggle,
 * preserving the other list. Depends on: check.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tui/tui_model_store.h"
#include "utils/string_utils.h"

extern void tui_model_store_test_set_alloc_fail(int nth_allocation);

static char path[256];

static void setup_store(void)
{
    snprintf(path, sizeof(path), "/tmp/test_model_store_%d.json", getpid());
    unlink(path);
}

static void teardown_store(void)
{
    unlink(path);
}

static void free_all(char **arr, int count)
{
    for (int i = 0; i < count; i++) free(arr[i]);
    free(arr);
}

START_TEST(test_load_missing_file_is_empty)
{
    char **recent = NULL;
    char **fav = NULL;
    int rc = 99, fc = 99;
    ck_assert_int_eq(tui_model_store_load(path, &recent, &rc, &fav, &fc), 0);
    ck_assert_int_eq(rc, 0);
    ck_assert_int_eq(fc, 0);
    ck_assert_ptr_nonnull(recent);
    ck_assert_ptr_nonnull(fav);
    free(recent);
    free(fav);
}
END_TEST

START_TEST(test_record_recent_front_inserts_and_dedups)
{
    ck_assert_int_eq(tui_model_store_record_recent(path, "a", 5), 0);
    ck_assert_int_eq(tui_model_store_record_recent(path, "b", 5), 0);
    ck_assert_int_eq(tui_model_store_record_recent(path, "a", 5), 0); /* dedup -> front */

    char **recent = NULL;
    char **fav = NULL;
    int rc = 0, fc = 0;
    ck_assert_int_eq(tui_model_store_load(path, &recent, &rc, &fav, &fc), 0);
    ck_assert_int_eq(rc, 2);
    ck_assert_str_eq(recent[0], "a");
    ck_assert_str_eq(recent[1], "b");
    free_all(recent, rc);
    free_all(fav, fc);
}
END_TEST

START_TEST(test_record_recent_caps)
{
    for (int i = 0; i < 6; i++)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "m%d", i);
        ck_assert_int_eq(tui_model_store_record_recent(path, buf, 3), 0);
    }
    char **recent = NULL;
    char **fav = NULL;
    int rc = 0, fc = 0;
    ck_assert_int_eq(tui_model_store_load(path, &recent, &rc, &fav, &fc), 0);
    ck_assert_int_eq(rc, 3);
    ck_assert_str_eq(recent[0], "m5");
    ck_assert_str_eq(recent[1], "m4");
    ck_assert_str_eq(recent[2], "m3");
    free_all(recent, rc);
    free_all(fav, fc);
}
END_TEST

START_TEST(test_favorite_toggle_and_preserves_recent)
{
    ck_assert_int_eq(tui_model_store_record_recent(path, "r1", 5), 0);
    int now = -1;
    ck_assert_int_eq(tui_model_store_toggle_favorite(path, "f1", &now), 0);
    ck_assert_int_eq(now, 1);
    ck_assert_int_eq(tui_model_store_toggle_favorite(path, "f1", &now), 0);
    ck_assert_int_eq(now, 0);

    char **recent = NULL;
    char **fav = NULL;
    int rc = 0, fc = 0;
    ck_assert_int_eq(tui_model_store_load(path, &recent, &rc, &fav, &fc), 0);
    ck_assert_int_eq(rc, 1);
    ck_assert_str_eq(recent[0], "r1"); /* recent untouched */
    ck_assert_int_eq(fc, 0);           /* favorite toggled off */
    free_all(recent, rc);
    free_all(fav, fc);
}
END_TEST

/* Regression: record_recent/toggle_favorite used `n` uninitialized when
 * the nr/nf calloc failed (goto done before n = 0) — an indeterminate
 * read in the free_list(nr, n) argument, caught by clang-tidy
 * (clang-diagnostic-sometimes-uninitialized). The crash is garbage-value
 * dependent, so the tests assert the deterministic contract: rc == -1 and
 * nothing committed. Load consumes 2 callocs (recent + favorites), so the
 * target list allocation is the 3rd. */
START_TEST(test_record_recent_nr_calloc_fail_is_clean)
{
    tui_model_store_test_set_alloc_fail(3);
    ck_assert_int_eq(tui_model_store_record_recent(path, "a", 5), -1);
    tui_model_store_test_set_alloc_fail(-1);

    char **recent = NULL;
    char **fav = NULL;
    int rc = 0, fc = 0;
    ck_assert_int_eq(tui_model_store_load(path, &recent, &rc, &fav, &fc), 0);
    ck_assert_int_eq(rc, 0); /* nothing committed */
    free_all(recent, rc);
    free_all(fav, fc);

    /* normal operation still works after the injected failure */
    ck_assert_int_eq(tui_model_store_record_recent(path, "a", 5), 0);
}
END_TEST

START_TEST(test_toggle_favorite_nf_calloc_fail_is_clean)
{
    /* no pre-populated file: load does exactly 2 callocs (recent + fav
     * empty arrays), so the nf calloc below is the 3rd and fails here */
    tui_model_store_test_set_alloc_fail(3);
    int now = -1;
    ck_assert_int_eq(tui_model_store_toggle_favorite(path, "f1", &now), -1);
    tui_model_store_test_set_alloc_fail(-1);

    char **recent = NULL;
    char **fav = NULL;
    int rc = 0, fc = 0;
    ck_assert_int_eq(tui_model_store_load(path, &recent, &rc, &fav, &fc), 0);
    ck_assert_int_eq(rc, 0); /* nothing committed */
    ck_assert_int_eq(fc, 0);
    free_all(recent, rc);
    free_all(fav, fc);

    /* normal operation still works after the injected failure */
    ck_assert_int_eq(tui_model_store_toggle_favorite(path, "f1", &now), 0);
    ck_assert_int_eq(now, 1);
}
END_TEST

/* Regression: tui_model_store_load leaked the initial empty recent/fav
 * arrays when a parse-time allocation failed (clang-analyzer-unix.Malloc).
 * On old code, the arrays stay non-NULL (and LSan flags the leak); on the
 * fixed code load returns -1 with both out-pointers NULLed. */
START_TEST(test_load_parse_alloc_fail_does_not_leak)
{
    FILE *fp = fopen(path, "w");
    ck_assert_ptr_nonnull(fp);
    fputs("{\"recent\":[\"a\",\"b\"]}", fp);
    fclose(fp);

    tui_model_store_test_set_alloc_fail(3); /* parse_list("recent") calloc */
    char **recent = NULL;
    char **fav = NULL;
    int rc = 99, fc = 99;
    ck_assert_int_eq(tui_model_store_load(path, &recent, &rc, &fav, &fc), -1);
    tui_model_store_test_set_alloc_fail(-1);
    ck_assert_ptr_null(recent);
    ck_assert_ptr_null(fav);
}
END_TEST

static Suite *tui_model_store_suite(void)
{
    Suite *s = suite_create("tui_model_store");
    TCase *tc = tcase_create("model_store");
    tcase_add_checked_fixture(tc, setup_store, teardown_store);
    tcase_add_test(tc, test_load_missing_file_is_empty);
    tcase_add_test(tc, test_record_recent_front_inserts_and_dedups);
    tcase_add_test(tc, test_record_recent_caps);
    tcase_add_test(tc, test_favorite_toggle_and_preserves_recent);
    tcase_add_test(tc, test_record_recent_nr_calloc_fail_is_clean);
    tcase_add_test(tc, test_toggle_favorite_nf_calloc_fail_is_clean);
    tcase_add_test(tc, test_load_parse_alloc_fail_does_not_leak);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(tui_model_store_suite());
    srunner_set_fork_status(sr, CK_FORK);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}