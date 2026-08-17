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

static Suite *tui_model_store_suite(void)
{
    Suite *s = suite_create("tui_model_store");
    TCase *tc = tcase_create("model_store");
    tcase_add_checked_fixture(tc, setup_store, teardown_store);
    tcase_add_test(tc, test_load_missing_file_is_empty);
    tcase_add_test(tc, test_record_recent_front_inserts_and_dedups);
    tcase_add_test(tc, test_record_recent_caps);
    tcase_add_test(tc, test_favorite_toggle_and_preserves_recent);
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