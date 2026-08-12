/*
 * test_tui_input.c - line editor model: insert/delete/cursor ops,
 * UTF-8 codepoint-atomic deletions, history walk, allocation-failure
 * preservation. Depends on: check.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tui/tui_input.h"

/* ---- fault-injection allocator shims (TUI_INPUT_TEST) ---- */
static int fail_at = -1;
static int call_count = 0;

void *tui_input_test_calloc(size_t nmemb, size_t size)
{
    call_count++;
    if (call_count == fail_at) return NULL;
    return calloc(nmemb, size);
}

char *tui_input_test_strdup(const char *s)
{
    call_count++;
    if (call_count == fail_at) return NULL;
    char *copy = malloc(strlen(s) + 1);
    if (!copy) return NULL;
    strcpy(copy, s);
    return copy;
}

static void set_fail_at(int n)
{
    fail_at = call_count + n;
}

/* ---- fixtures ---- */
static TuiInput *in;

static void setup_input(void)
{
    fail_at = -1;
    call_count = 0;
    in = tui_input_create(8);
    ck_assert_ptr_nonnull(in);
}

static void teardown_input(void)
{
    tui_input_destroy(in);
    in = NULL;
}

/* ---- insert / cursor ---- */

START_TEST(test_insert_basic_and_cursor_moves)
{
    ck_assert_int_eq(tui_input_insert(in, "hello"), 0);
    ck_assert_str_eq(tui_input_text(in), "hello");
    ck_assert_int_eq(tui_input_len(in), 5);
    ck_assert_int_eq(tui_input_cursor(in), 5);
}
END_TEST

START_TEST(test_insert_mid_buffer)
{
    ck_assert_int_eq(tui_input_insert(in, "abcd"), 0);
    tui_input_home(in);
    ck_assert_int_eq(tui_input_insert(in, "XY"), 0);
    ck_assert_str_eq(tui_input_text(in), "XYabcd");
    tui_input_move(in, 1); /* cursor now after 'a' */
    ck_assert_int_eq(tui_input_insert(in, "!"), 0);
    ck_assert_str_eq(tui_input_text(in), "XYa!bcd");
}
END_TEST

START_TEST(test_backspace_delete_and_edges)
{
    ck_assert_int_eq(tui_input_insert(in, "abc"), 0);
    ck_assert_int_eq(tui_input_backspace(in), 1);
    ck_assert_str_eq(tui_input_text(in), "ab");
    ck_assert_int_eq(tui_input_delete(in), 0); /* at end: no-op */
    tui_input_home(in);
    ck_assert_int_eq(tui_input_backspace(in), 0); /* at start: no-op */
    ck_assert_int_eq(tui_input_delete(in), 1);
    ck_assert_str_eq(tui_input_text(in), "b");
}
END_TEST

START_TEST(test_utf8_codepoint_atomic_deletion)
{
    /* "héllo" = h, é (2 bytes), l, l, o */
    ck_assert_int_eq(tui_input_insert(in, "h\xc3\xa9llo"), 0);
    ck_assert_int_eq(tui_input_len(in), 6);
    /* backspace removes the whole é, not one byte */
    tui_input_end(in);
    ck_assert_int_eq(tui_input_backspace(in), 1); /* 'o' */
    ck_assert_int_eq(tui_input_backspace(in), 1); /* 'l' */
    ck_assert_int_eq(tui_input_backspace(in), 1); /* second 'l' */
    ck_assert_int_eq(tui_input_backspace(in), 2); /* the 2-byte é */
    ck_assert_str_eq(tui_input_text(in), "h");
    ck_assert_int_eq(tui_input_cursor(in), 1);
}
END_TEST

START_TEST(test_cursor_move_skips_continuation_bytes)
{
    ck_assert_int_eq(tui_input_insert(in, "a\xc3\xa9" "b"), 0); /* a, é, b */
    tui_input_home(in);
    tui_input_move(in, 1); /* over 'a' */
    ck_assert_int_eq(tui_input_cursor(in), 1);
    tui_input_move(in, 1); /* over the whole é */
    ck_assert_int_eq(tui_input_cursor(in), 3);
    tui_input_move(in, 1);
    ck_assert_int_eq(tui_input_cursor(in), 4);
    tui_input_move(in, -2); /* back over é + b */
    ck_assert_int_eq(tui_input_cursor(in), 1);
}
END_TEST

START_TEST(test_home_end_clear)
{
    ck_assert_int_eq(tui_input_insert(in, "abcdef"), 0);
    tui_input_home(in);
    ck_assert_int_eq(tui_input_cursor(in), 0);
    tui_input_end(in);
    ck_assert_int_eq(tui_input_cursor(in), 6);
    tui_input_clear(in);
    ck_assert_str_eq(tui_input_text(in), "");
    ck_assert_int_eq(tui_input_cursor(in), 0);
}
END_TEST

START_TEST(test_delete_word)
{
    ck_assert_int_eq(tui_input_insert(in, "one two three"), 0);
    tui_input_end(in);
    ck_assert_int_eq(tui_input_delete_word(in), 5); /* "three" */
    ck_assert_str_eq(tui_input_text(in), "one two ");
    /* Ctrl-W kills the word together with its preceding space */
    ck_assert_int_eq(tui_input_delete_word(in), 4); /* "two " */
    ck_assert_str_eq(tui_input_text(in), "one ");
    /* at start: no-op */
    tui_input_home(in);
    ck_assert_int_eq(tui_input_delete_word(in), 0);
}
END_TEST

START_TEST(test_empty_insert_noop)
{
    ck_assert_int_eq(tui_input_insert(in, ""), 0);
    ck_assert_str_eq(tui_input_text(in), "");
}
END_TEST

/* ---- history ---- */

START_TEST(test_submit_clears_buffer_and_returns_owned)
{
    ck_assert_int_eq(tui_input_insert(in, "question one"), 0);
    char *line = tui_input_submit(in);
    ck_assert_ptr_nonnull(line);
    ck_assert_str_eq(line, "question one");
    free(line);
    ck_assert_str_eq(tui_input_text(in), "");
    ck_assert_int_eq(tui_input_cursor(in), 0);
}
END_TEST

START_TEST(test_empty_submit_returns_null)
{
    ck_assert_ptr_null(tui_input_submit(in));
}
END_TEST

START_TEST(test_history_back_forward_walk)
{
    ck_assert_int_eq(tui_input_insert(in, "first line"), 0);
    char *l1 = tui_input_submit(in);
    ck_assert_ptr_nonnull(l1);
    free(l1);
    ck_assert_int_eq(tui_input_insert(in, "second"), 0);
    char *l2 = tui_input_submit(in);
    ck_assert_ptr_nonnull(l2);
    free(l2);

    /* start a fresh live edit, then walk into history */
    ck_assert_int_eq(tui_input_insert(in, "my draft"), 0);
    const char *walk = tui_input_history_back(in);
    ck_assert_ptr_nonnull(walk);
    ck_assert_str_eq(walk, "second"); /* newest first */
    walk = tui_input_history_back(in);
    ck_assert_str_eq(walk, "first line");
    /* at the oldest entry */
    ck_assert_ptr_null(tui_input_history_back(in));
    /* forward again */
    walk = tui_input_history_forward(in);
    ck_assert_str_eq(walk, "second");
    /* forward past the newest entry restores the live draft */
    walk = tui_input_history_forward(in);
    ck_assert_ptr_nonnull(walk);
    ck_assert_str_eq(walk, "my draft");
    ck_assert_ptr_null(tui_input_history_forward(in));
}
END_TEST

START_TEST(test_history_cap_and_dedupe)
{
    TuiInput *small = tui_input_create(2);
    ck_assert_ptr_nonnull(small);
    /* duplicate consecutive submissions are not stored twice */
    char *l = tui_input_submit(small);
    free(l);
    ck_assert_ptr_null(tui_input_submit(small)); /* empty line: no-op */

    for (int i = 0; i < 5; i++)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "line%d", i);
        ck_assert_int_eq(tui_input_insert(small, buf), 0);
        char *line = tui_input_submit(small);
        ck_assert_ptr_nonnull(line);
        free(line);
    }
    /* only the last 2 survive */
    const char *w1 = tui_input_history_back(small);
    ck_assert_str_eq(w1, "line4");
    const char *w2 = tui_input_history_back(small);
    ck_assert_str_eq(w2, "line3");
    ck_assert_ptr_null(tui_input_history_back(small));
    tui_input_destroy(small);
}
END_TEST

START_TEST(test_history_zero_cap_disabled)
{
    TuiInput *no_hist = tui_input_create(0);
    ck_assert_ptr_nonnull(no_hist);
    char *l = tui_input_submit(no_hist);
    free(l);
    ck_assert_ptr_null(tui_input_history_back(no_hist));
    tui_input_destroy(no_hist);
}
END_TEST

START_TEST(test_set_text_replaces_buffer)
{
    ck_assert_int_eq(tui_input_insert(in, "old"), 0);
    ck_assert_int_eq(tui_input_set_text(in, "new content"), 0);
    ck_assert_str_eq(tui_input_text(in), "new content");
    ck_assert_int_eq(tui_input_cursor(in), 11);
    ck_assert_int_eq(tui_input_set_text(in, NULL), 0);
    ck_assert_str_eq(tui_input_text(in), "");
}
END_TEST

/* ---- allocation-failure preservation ---- */

START_TEST(test_insert_failure_preserves_buffer)
{
    ck_assert_int_eq(tui_input_insert(in, "stable"), 0);
    /* 100 bytes forces growth past the initial 64-byte capacity */
    char big[100];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    /* the growth calloc fails: nothing must change */
    set_fail_at(1);
    ck_assert_int_eq(tui_input_insert(in, big), -1);
    ck_assert_str_eq(tui_input_text(in), "stable");
    ck_assert_int_eq(tui_input_cursor(in), 6);
    /* and the editor still works afterwards */
    fail_at = -1;
    ck_assert_int_eq(tui_input_insert(in, "!"), 0);
    ck_assert_str_eq(tui_input_text(in), "stable!");
}
END_TEST

START_TEST(test_submit_failure_keeps_buffer)
{
    ck_assert_int_eq(tui_input_insert(in, "keep me"), 0);
    set_fail_at(1); /* the submitted copy's str_dup fails */
    char *line = tui_input_submit(in);
    ck_assert_ptr_null(line);
    ck_assert_str_eq(tui_input_text(in), "keep me");
    fail_at = -1;
    char *ok = tui_input_submit(in);
    ck_assert_ptr_nonnull(ok);
    ck_assert_str_eq(ok, "keep me");
    free(ok);
}
END_TEST

START_TEST(test_set_text_failure_keeps_content)
{
    ck_assert_int_eq(tui_input_insert(in, "old content"), 0);
    /* the replacement exceeds the initial 64-byte capacity, forcing growth */
    char big[200];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    set_fail_at(1);
    ck_assert_int_eq(tui_input_set_text(in, big), -1);
    ck_assert_str_eq(tui_input_text(in), "old content");
}
END_TEST

static Suite *suite(void)
{
    Suite *s = suite_create("tui_input");
    TCase *tc = tcase_create("input");
    tcase_add_checked_fixture(tc, setup_input, teardown_input);
    tcase_add_test(tc, test_insert_basic_and_cursor_moves);
    tcase_add_test(tc, test_insert_mid_buffer);
    tcase_add_test(tc, test_backspace_delete_and_edges);
    tcase_add_test(tc, test_utf8_codepoint_atomic_deletion);
    tcase_add_test(tc, test_cursor_move_skips_continuation_bytes);
    tcase_add_test(tc, test_home_end_clear);
    tcase_add_test(tc, test_delete_word);
    tcase_add_test(tc, test_empty_insert_noop);
    tcase_add_test(tc, test_submit_clears_buffer_and_returns_owned);
    tcase_add_test(tc, test_empty_submit_returns_null);
    tcase_add_test(tc, test_history_back_forward_walk);
    tcase_add_test(tc, test_history_cap_and_dedupe);
    tcase_add_test(tc, test_history_zero_cap_disabled);
    tcase_add_test(tc, test_set_text_replaces_buffer);
    tcase_add_test(tc, test_insert_failure_preserves_buffer);
    tcase_add_test(tc, test_submit_failure_keeps_buffer);
    tcase_add_test(tc, test_set_text_failure_keeps_content);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? 0 : 1;
}
