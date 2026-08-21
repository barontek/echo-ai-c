/*
 * test_tui_prompt_store.c - persistent prompt history and stash:
 * append/cap/dedup, load ordering, stash push/pop/list/remove, malformed
 * line skipping, missing-file handling. Depends on: check.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tui/tui_prompt_store.h"
#include "utils/string_utils.h"

static char path[256];

static void setup_store(void)
{
    snprintf(path, sizeof(path), "/tmp/test_prompt_store_%d.jsonl", getpid());
    unlink(path);
}

static void teardown_store(void)
{
    unlink(path);
}

static void write_raw(const char *content)
{
    FILE *fp = fopen(path, "w");
    ck_assert_ptr_nonnull(fp);
    fwrite(content, 1, strlen(content), fp);
    fclose(fp);
}

static char **load_all(int *count)
{
    char **out = NULL;
    int n = 0;
    ck_assert_int_eq(tui_prompt_store_history_load(path, &out, &n), 0);
    *count = n;
    return out;
}

static void free_all(char **arr, int count)
{
    for (int i = 0; i < count; i++) free(arr[i]);
    free(arr);
}

START_TEST(test_history_append_and_load_order)
{
    ck_assert_int_eq(tui_prompt_store_history_append(path, "first", NULL, 50), 0);
    ck_assert_int_eq(tui_prompt_store_history_append(path, "second", "shell", 50), 0);

    int count = 0;
    char **arr = load_all(&count);
    ck_assert_int_eq(count, 2);
    ck_assert_str_eq(arr[0], "first");
    ck_assert_str_eq(arr[1], "second");
    free_all(arr, count);
}
END_TEST

START_TEST(test_history_dedups_consecutive)
{
    ck_assert_int_eq(tui_prompt_store_history_append(path, "same", NULL, 50), 0);
    ck_assert_int_eq(tui_prompt_store_history_append(path, "same", NULL, 50), 0);
    ck_assert_int_eq(tui_prompt_store_history_append(path, "other", NULL, 50), 0);

    int count = 0;
    char **arr = load_all(&count);
    ck_assert_int_eq(count, 2);
    ck_assert_str_eq(arr[0], "same");
    ck_assert_str_eq(arr[1], "other");
    free_all(arr, count);
}
END_TEST

START_TEST(test_history_cap_trims_oldest)
{
    for (int i = 0; i < 5; i++)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "msg%d", i);
        ck_assert_int_eq(tui_prompt_store_history_append(path, buf, NULL, 3), 0);
    }
    int count = 0;
    char **arr = load_all(&count);
    ck_assert_int_eq(count, 3);
    ck_assert_str_eq(arr[0], "msg2");
    ck_assert_str_eq(arr[1], "msg3");
    ck_assert_str_eq(arr[2], "msg4");
    free_all(arr, count);
}
END_TEST

START_TEST(test_history_missing_file_is_empty)
{
    int count = 99;
    char **arr = load_all(&count);
    ck_assert_int_eq(count, 0);
    ck_assert_ptr_nonnull(arr); /* caller frees; empty array */
    free(arr);
}
END_TEST

START_TEST(test_history_skips_malformed_lines)
{
    write_raw("{\"input\":\"good\"}\nnot json\n{\"noinput\":true}\n{\"input\":\"also good\"}\n");
    int count = 0;
    char **arr = load_all(&count);
    ck_assert_int_eq(count, 2);
    ck_assert_str_eq(arr[0], "good");
    ck_assert_str_eq(arr[1], "also good");
    free_all(arr, count);
}
END_TEST

START_TEST(test_history_handles_newlines_in_input)
{
    ck_assert_int_eq(tui_prompt_store_history_append(path, "line one\nline two", NULL, 50), 0);
    int count = 0;
    char **arr = load_all(&count);
    ck_assert_int_eq(count, 1);
    ck_assert_str_eq(arr[0], "line one\nline two");
    free_all(arr, count);
}
END_TEST

START_TEST(test_stash_push_pop_lifo)
{
    ck_assert_int_eq(tui_prompt_store_stash_push(path, "draft A", 50), 0);
    ck_assert_int_eq(tui_prompt_store_stash_push(path, "draft B", 50), 0);

    char *popped = NULL;
    ck_assert_int_eq(tui_prompt_store_stash_pop(path, &popped), 0);
    ck_assert_ptr_nonnull(popped);
    ck_assert_str_eq(popped, "draft B");
    free(popped);

    /* B is gone; A remains */
    int count = 0;
    char **arr = NULL;
    ck_assert_int_eq(tui_prompt_store_stash_list(path, &arr, &count), 0);
    ck_assert_int_eq(count, 1);
    ck_assert_str_eq(arr[0], "draft A");
    free_all(arr, count);
}
END_TEST

START_TEST(test_stash_pop_empty)
{
    char *popped = (char *)(size_t)1;
    ck_assert_int_eq(tui_prompt_store_stash_pop(path, &popped), 0);
    ck_assert_ptr_null(popped);
}
END_TEST

START_TEST(test_stash_list_newest_first)
{
    ck_assert_int_eq(tui_prompt_store_stash_push(path, "old", 50), 0);
    ck_assert_int_eq(tui_prompt_store_stash_push(path, "new", 50), 0);

    int count = 0;
    char **arr = NULL;
    ck_assert_int_eq(tui_prompt_store_stash_list(path, &arr, &count), 0);
    ck_assert_int_eq(count, 2);
    ck_assert_str_eq(arr[0], "new");
    ck_assert_str_eq(arr[1], "old");
    free_all(arr, count);
}
END_TEST

START_TEST(test_stash_remove_by_index)
{
    ck_assert_int_eq(tui_prompt_store_stash_push(path, "A", 50), 0);
    ck_assert_int_eq(tui_prompt_store_stash_push(path, "B", 50), 0);
    ck_assert_int_eq(tui_prompt_store_stash_push(path, "C", 50), 0);

    /* remove "B" (index 1 in newest-first: C, B, A) */
    ck_assert_int_eq(tui_prompt_store_stash_remove(path, 1), 0);

    int count = 0;
    char **arr = NULL;
    ck_assert_int_eq(tui_prompt_store_stash_list(path, &arr, &count), 0);
    ck_assert_int_eq(count, 2);
    ck_assert_str_eq(arr[0], "C");
    ck_assert_str_eq(arr[1], "A");
    free_all(arr, count);

    ck_assert_int_eq(tui_prompt_store_stash_remove(path, 5), -1); /* OOB */
}
END_TEST

static Suite *tui_prompt_store_suite(void)
{
    Suite *s = suite_create("tui_prompt_store");
    TCase *tc = tcase_create("prompt_store");
    tcase_add_checked_fixture(tc, setup_store, teardown_store);
    tcase_add_test(tc, test_history_append_and_load_order);
    tcase_add_test(tc, test_history_dedups_consecutive);
    tcase_add_test(tc, test_history_cap_trims_oldest);
    tcase_add_test(tc, test_history_missing_file_is_empty);
    tcase_add_test(tc, test_history_skips_malformed_lines);
    tcase_add_test(tc, test_history_handles_newlines_in_input);
    tcase_add_test(tc, test_stash_push_pop_lifo);
    tcase_add_test(tc, test_stash_pop_empty);
    tcase_add_test(tc, test_stash_list_newest_first);
    tcase_add_test(tc, test_stash_remove_by_index);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(tui_prompt_store_suite());
    srunner_set_fork_status(sr, CK_FORK);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
