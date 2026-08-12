/*
 * test_tui_stream.c - streamed-chunk classification: marker splitting,
 * separator-whitespace trimming, state transitions, and the no-op marker
 * deltas the providers emit. Regression for the "3 blank lines between
 * thinking and reply" bug. Depends on: check.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <string.h>

#include "tui/tui_stream.h"

/* Helper: render a split as "kind:text" list into buf for easy asserts. */
static void parts_dump(const TuiStreamPart *parts, int n, char *buf, size_t cap)
{
    size_t o = 0;
    for (int i = 0; i < n && o + 4 < cap; i++)
    {
        o += (size_t)snprintf(buf + o, cap - o, "%c:",
                              parts[i].kind == TUI_STREAM_PART_THINK ? 'T' : 'C');
        for (size_t k = 0; k < parts[i].len && o + 1 < cap; k++)
            buf[o++] = parts[i].start[k];
        buf[o] = '\0';
    }
    buf[o] = '\0';
}

START_TEST(test_plain_content_chunk_outside_think)
{
    TuiStreamPart parts[2];
    int out = -1;
    int n = tui_stream_split("hello there", 0, parts, &out);
    ck_assert_int_eq(n, 1);
    ck_assert_int_eq(out, 0);
    char buf[64];
    parts_dump(parts, n, buf, sizeof(buf));
    ck_assert_str_eq(buf, "C:hello there");
}
END_TEST

START_TEST(test_plain_chunk_inside_think)
{
    TuiStreamPart parts[2];
    int out = -1;
    int n = tui_stream_split("reasoning text", 1, parts, &out);
    ck_assert_int_eq(n, 1);
    ck_assert_int_eq(out, 1);
    char buf[64];
    parts_dump(parts, n, buf, sizeof(buf));
    ck_assert_str_eq(buf, "T:reasoning text");
}
END_TEST

START_TEST(test_opening_marker_delta_is_noop)
{
    /* Provider convention: "<think>\n" — marker + newline only */
    TuiStreamPart parts[2];
    int out = -1;
    int n = tui_stream_split("<think>\n", 0, parts, &out);
    ck_assert_int_eq(n, 0);
    ck_assert_int_eq(out, 1); /* state flips, nothing emitted */
}
END_TEST

START_TEST(test_closing_marker_delta_is_noop)
{
    /* Provider convention: "\n</think>\n\n" — the "3 spaces" regression */
    TuiStreamPart parts[2];
    int out = -1;
    int n = tui_stream_split("\n</think>\n\n", 1, parts, &out);
    ck_assert_int_eq(n, 0);
    ck_assert_int_eq(out, 0);
}
END_TEST

START_TEST(test_open_marker_with_inline_text)
{
    TuiStreamPart parts[2];
    int out = -1;
    int n = tui_stream_split("<think>Let me think", 0, parts, &out);
    ck_assert_int_eq(n, 1);
    ck_assert_int_eq(out, 1);
    char buf[64];
    parts_dump(parts, n, buf, sizeof(buf));
    ck_assert_str_eq(buf, "T:Let me think");
}
END_TEST

START_TEST(test_close_marker_splits_think_and_content)
{
    TuiStreamPart parts[2];
    int out = -1;
    int n = tui_stream_split("some thinking\n</think>And the answer", 1, parts, &out);
    ck_assert_int_eq(n, 2);
    ck_assert_int_eq(out, 0);
    char buf[64];
    parts_dump(parts, n, buf, sizeof(buf));
    ck_assert_str_eq(buf, "T:some thinkingC:And the answer");
}
END_TEST

START_TEST(test_close_trimmed_trailing_whitespace)
{
    TuiStreamPart parts[2];
    int out = -1;
    int n = tui_stream_split("thoughts   \n\n</think>", 1, parts, &out);
    ck_assert_int_eq(n, 1); /* pre-part trimmed to "thoughts" */
    ck_assert_int_eq(out, 0);
    char buf[64];
    parts_dump(parts, n, buf, sizeof(buf));
    ck_assert_str_eq(buf, "T:thoughts");
}
END_TEST

START_TEST(test_open_with_pre_content)
{
    TuiStreamPart parts[2];
    int out = -1;
    int n = tui_stream_split("intro text <think>deep think", 0, parts, &out);
    ck_assert_int_eq(n, 2);
    ck_assert_int_eq(out, 1);
    char buf[64];
    parts_dump(parts, n, buf, sizeof(buf));
    ck_assert_str_eq(buf, "C:intro textT:deep think");
}
END_TEST

START_TEST(test_empty_chunk)
{
    TuiStreamPart parts[2];
    int out = -1;
    int n = tui_stream_split("", 0, parts, &out);
    ck_assert_int_eq(n, 0);
    ck_assert_int_eq(out, 0);
}
END_TEST

START_TEST(test_null_args_are_safe)
{
    TuiStreamPart parts[2];
    ck_assert_int_eq(tui_stream_split(NULL, 0, parts, NULL), 0);
}
END_TEST

static Suite *suite(void)
{
    Suite *s = suite_create("tui_stream");
    TCase *tc = tcase_create("classify");
    tcase_add_test(tc, test_plain_content_chunk_outside_think);
    tcase_add_test(tc, test_plain_chunk_inside_think);
    tcase_add_test(tc, test_opening_marker_delta_is_noop);
    tcase_add_test(tc, test_closing_marker_delta_is_noop);
    tcase_add_test(tc, test_open_marker_with_inline_text);
    tcase_add_test(tc, test_close_marker_splits_think_and_content);
    tcase_add_test(tc, test_close_trimmed_trailing_whitespace);
    tcase_add_test(tc, test_open_with_pre_content);
    tcase_add_test(tc, test_empty_chunk);
    tcase_add_test(tc, test_null_args_are_safe);
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
