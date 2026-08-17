/*
 * test_tui_session_store.c - session pins: load/save round-trip, toggle
 * pin/unpin, front-insert ordering, slot resolution (pins first, then
 * recency), missing-file and malformed handling. Depends on: check.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tui/tui_session_store.h"
#include "utils/string_utils.h"

static char path[256];

static void setup_pins(void)
{
    snprintf(path, sizeof(path), "/tmp/test_session_pins_%d.json", getpid());
    unlink(path);
}

static void teardown_pins(void)
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
    char **pins = NULL;
    int count = 99;
    ck_assert_int_eq(tui_session_store_load_pins(path, &pins, &count), 0);
    ck_assert_int_eq(count, 0);
    ck_assert_ptr_nonnull(pins);
    free(pins);
}
END_TEST

START_TEST(test_save_then_load_round_trip)
{
    const char *ids[] = { "s1", "s2", "s3" };
    ck_assert_int_eq(tui_session_store_save_pins(path, ids, 3), 0);

    char **pins = NULL;
    int count = 0;
    ck_assert_int_eq(tui_session_store_load_pins(path, &pins, &count), 0);
    ck_assert_int_eq(count, 3);
    ck_assert_str_eq(pins[0], "s1");
    ck_assert_str_eq(pins[1], "s2");
    ck_assert_str_eq(pins[2], "s3");
    free_all(pins, count);
}
END_TEST

START_TEST(test_toggle_pin_inserts_front)
{
    ck_assert_int_eq(tui_session_store_toggle_pin(path, "s1", NULL), 0);
    ck_assert_int_eq(tui_session_store_toggle_pin(path, "s2", NULL), 0);

    char **pins = NULL;
    int count = 0;
    ck_assert_int_eq(tui_session_store_load_pins(path, &pins, &count), 0);
    ck_assert_int_eq(count, 2);
    /* s2 pinned last -> newest pin is first */
    ck_assert_str_eq(pins[0], "s2");
    ck_assert_str_eq(pins[1], "s1");
    free_all(pins, count);
}
END_TEST

START_TEST(test_toggle_unpin_removes)
{
    ck_assert_int_eq(tui_session_store_toggle_pin(path, "s1", NULL), 0);
    ck_assert_int_eq(tui_session_store_toggle_pin(path, "s2", NULL), 0);

    int now = -1;
    ck_assert_int_eq(tui_session_store_toggle_pin(path, "s1", &now), 0);
    ck_assert_int_eq(now, 0);

    char **pins = NULL;
    int count = 0;
    ck_assert_int_eq(tui_session_store_load_pins(path, &pins, &count), 0);
    ck_assert_int_eq(count, 1);
    ck_assert_str_eq(pins[0], "s2");
    free_all(pins, count);
}
END_TEST

START_TEST(test_resolve_slot_pins_first)
{
    const char *pinned[] = { "s9" };
    const char *all[] = { "s1", "s2", "s3", "s9" };
    char buf[64];

    /* pinned session occupies slot 1 */
    ck_assert_int_eq(tui_session_store_resolve_slot(pinned, 1, all, 4, 1, buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "s9");
    /* non-pinned sessions fill the remaining slots in order */
    ck_assert_int_eq(tui_session_store_resolve_slot(pinned, 1, all, 4, 2, buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "s1");
    ck_assert_int_eq(tui_session_store_resolve_slot(pinned, 1, all, 4, 4, buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "s3");
    ck_assert_int_eq(tui_session_store_resolve_slot(pinned, 1, all, 4, 5, buf, sizeof(buf)), 0);
}
END_TEST

START_TEST(test_resolve_slot_skips_stale_pin)
{
    /* a pinned id no longer in the session list is skipped */
    const char *pinned[] = { "gone", "s2" };
    const char *all[] = { "s1", "s2", "s3" };
    char buf[64];

    ck_assert_int_eq(tui_session_store_resolve_slot(pinned, 2, all, 3, 1, buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "s2");
    ck_assert_int_eq(tui_session_store_resolve_slot(pinned, 2, all, 3, 2, buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "s1");
}
END_TEST

START_TEST(test_resolve_slot_zero_and_negative)
{
    const char *all[] = { "s1" };
    char buf[64];
    ck_assert_int_eq(tui_session_store_resolve_slot(NULL, 0, all, 1, 0, buf, sizeof(buf)), 0);
    ck_assert_int_eq(tui_session_store_resolve_slot(NULL, 0, all, 1, -3, buf, sizeof(buf)), 0);
}
END_TEST

static Suite *tui_session_store_suite(void)
{
    Suite *s = suite_create("tui_session_store");
    TCase *tc = tcase_create("session_store");
    tcase_add_checked_fixture(tc, setup_pins, teardown_pins);
    tcase_add_test(tc, test_load_missing_file_is_empty);
    tcase_add_test(tc, test_save_then_load_round_trip);
    tcase_add_test(tc, test_toggle_pin_inserts_front);
    tcase_add_test(tc, test_toggle_unpin_removes);
    tcase_add_test(tc, test_resolve_slot_pins_first);
    tcase_add_test(tc, test_resolve_slot_skips_stale_pin);
    tcase_add_test(tc, test_resolve_slot_zero_and_negative);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(tui_session_store_suite());
    srunner_set_fork_status(sr, CK_FORK);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}