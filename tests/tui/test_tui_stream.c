/*
 * test_tui_stream.c - stateful chunk classification: marker splitting,
 * separator-whitespace trimming, state transitions, the no-op marker
 * deltas providers emit, markers split across chunk boundaries (carry),
 * multi-marker chunks, and end-of-stream flush. Regression for the
 * "3 blank lines between thinking and reply" bug. Depends on: check.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <string.h>

#include "tui/tui_stream.h"

/* Helper: render parts as "kind:text" list into buf for easy asserts. */
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

/* Feed one chunk and dump the result into buf. */
static int feed_dump(TuiStreamClassifier *cls, const char *chunk,
                     char *buf, size_t cap)
{
    TuiStreamPart parts[4];
    int n = tui_stream_classifier_feed(cls, chunk, parts, 4);
    parts_dump(parts, n, buf, cap);
    return n;
}

START_TEST(test_plain_content_chunk_outside_think)
{
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    ck_assert_int_eq(feed_dump(cls, "hello there", buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "C:hello there");
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_plain_chunk_inside_think)
{
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    (void)feed_dump(cls, "<think>", buf, sizeof(buf)); /* open */
    ck_assert_int_eq(feed_dump(cls, "reasoning text", buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "T:reasoning text");
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_opening_marker_delta_is_noop)
{
    /* Provider convention: "<think>\n" — marker + newline only */
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    ck_assert_int_eq(feed_dump(cls, "<think>\n", buf, sizeof(buf)), 0);
    ck_assert_str_eq(buf, "");
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_closing_marker_delta_is_noop)
{
    /* Provider convention: "\n</think>\n\n" — the "3 spaces" regression */
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    (void)feed_dump(cls, "<think>", buf, sizeof(buf));
    ck_assert_int_eq(feed_dump(cls, "\n</think>\n\n", buf, sizeof(buf)), 0);
    ck_assert_str_eq(buf, "");
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_open_marker_with_inline_text)
{
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    ck_assert_int_eq(feed_dump(cls, "<think>Let me think", buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "T:Let me think");
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_close_marker_splits_think_and_content)
{
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    (void)feed_dump(cls, "<think>", buf, sizeof(buf));
    ck_assert_int_eq(feed_dump(cls, "some thinking\n</think>And the answer",
                               buf, sizeof(buf)), 2);
    ck_assert_str_eq(buf, "T:some thinkingC:And the answer");
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_close_trimmed_trailing_whitespace)
{
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    (void)feed_dump(cls, "<think>", buf, sizeof(buf));
    ck_assert_int_eq(feed_dump(cls, "thoughts   \n\n</think>", buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "T:thoughts");
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_open_with_pre_content)
{
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    ck_assert_int_eq(feed_dump(cls, "intro text <think>deep think",
                               buf, sizeof(buf)), 2);
    ck_assert_str_eq(buf, "C:intro textT:deep think");
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_empty_chunk_is_noop)
{
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    ck_assert_int_eq(feed_dump(cls, "", buf, sizeof(buf)), 0);
    /* an empty chunk must not flush a pending carry */
    (void)feed_dump(cls, "<thi", buf, sizeof(buf));
    ck_assert_str_eq(buf, "");
    ck_assert_int_eq(feed_dump(cls, "", buf, sizeof(buf)), 0);
    ck_assert_int_eq(feed_dump(cls, "nk>", buf, sizeof(buf)), 0);
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_null_args_are_safe)
{
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    TuiStreamPart parts[4];
    ck_assert_int_eq(tui_stream_classifier_feed(NULL, "x", parts, 4), 0);
    ck_assert_int_eq(tui_stream_classifier_feed(cls, NULL, parts, 4), 0);
    ck_assert_int_eq(tui_stream_classifier_feed(cls, "x", NULL, 4), 0);
    ck_assert_int_eq(tui_stream_classifier_feed(cls, "x", parts, 0), 0);
    ck_assert_int_eq(tui_stream_classifier_flush(NULL, NULL), 0);
    tui_stream_classifier_destroy(NULL);
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_open_marker_split_across_chunks)
{
    /* "<thi" + "nk>" must resolve into one open transition, no output */
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    ck_assert_int_eq(feed_dump(cls, "intro <thi", buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "C:intro "); /* "<thi" is held back */
    ck_assert_int_eq(feed_dump(cls, "nk>thinking", buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "T:thinking");
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_close_marker_split_across_chunks)
{
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    (void)feed_dump(cls, "<think>", buf, sizeof(buf));
    ck_assert_int_eq(feed_dump(cls, "reasoning</thi", buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "T:reasoning");
    ck_assert_int_eq(feed_dump(cls, "nk>answer", buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "C:answer");
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_false_marker_prefix_is_emitted_once)
{
    /* "<" followed by non-marker text is content, emitted exactly once */
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    ck_assert_int_eq(feed_dump(cls, "a <", buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "C:a "); /* "<" held back, "a " emitted */
    ck_assert_int_eq(feed_dump(cls, "div> tag", buf, sizeof(buf)), 1);
    ck_assert_str_eq(buf, "C:<div> tag"); /* resolved: no duplication */
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_both_markers_in_one_chunk)
{
    /* content + think + content in a single delta; the pre-marker
     * segment is trailing-trimmed like every marker-adjacent segment */
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[96];
    ck_assert_int_eq(feed_dump(cls, "pre <think>mid</think>post",
                               buf, sizeof(buf)), 3);
    ck_assert_str_eq(buf, "C:preT:midC:post");
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_alternating_markers_across_chunks)
{
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[96];
    ck_assert_int_eq(feed_dump(cls, "a<think>b</think>c<think>",
                               buf, sizeof(buf)), 3);
    ck_assert_str_eq(buf, "C:aT:bC:c");
    ck_assert_int_eq(feed_dump(cls, "d</think>e", buf, sizeof(buf)), 2);
    ck_assert_str_eq(buf, "T:dC:e");
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_flush_emits_unresolved_carry)
{
    /* end of stream with a partial marker: the tail must not be lost */
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    (void)feed_dump(cls, "done <thi", buf, sizeof(buf));
    TuiStreamPart tail;
    ck_assert_int_eq(tui_stream_classifier_flush(cls, &tail), 1);
    ck_assert_int_eq(tail.kind, TUI_STREAM_PART_CONTENT);
    ck_assert_int_eq(tail.len, 4);
    ck_assert(memcmp(tail.start, "<thi", 4) == 0);
    ck_assert_int_eq(tui_stream_classifier_flush(cls, &tail), 0); /* once */
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_flush_inside_think_uses_think_kind)
{
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    (void)feed_dump(cls, "<think>", buf, sizeof(buf));
    (void)feed_dump(cls, "tail</th", buf, sizeof(buf));
    TuiStreamPart tail;
    ck_assert_int_eq(tui_stream_classifier_flush(cls, &tail), 1);
    ck_assert_int_eq(tail.kind, TUI_STREAM_PART_THINK);
    tui_stream_classifier_destroy(cls);
}
END_TEST

START_TEST(test_flush_nothing_pending)
{
    TuiStreamClassifier *cls = tui_stream_classifier_create();
    ck_assert_ptr_nonnull(cls);
    char buf[64];
    (void)feed_dump(cls, "complete text", buf, sizeof(buf));
    TuiStreamPart tail;
    ck_assert_int_eq(tui_stream_classifier_flush(cls, &tail), 0);
    tui_stream_classifier_destroy(cls);
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
    tcase_add_test(tc, test_empty_chunk_is_noop);
    tcase_add_test(tc, test_null_args_are_safe);
    tcase_add_test(tc, test_open_marker_split_across_chunks);
    tcase_add_test(tc, test_close_marker_split_across_chunks);
    tcase_add_test(tc, test_false_marker_prefix_is_emitted_once);
    tcase_add_test(tc, test_both_markers_in_one_chunk);
    tcase_add_test(tc, test_alternating_markers_across_chunks);
    tcase_add_test(tc, test_flush_emits_unresolved_carry);
    tcase_add_test(tc, test_flush_inside_think_uses_think_kind);
    tcase_add_test(tc, test_flush_nothing_pending);
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
