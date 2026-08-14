/*
 * test_tool_args.c - compaction of tool-call argument JSON into the
 * one-line header summary, including UTF-8 truncation and degradation
 * on unparseable input. Depends on: check, cJSON.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tui/tool_args.h"

START_TEST(test_compact_object_strings_bare)
{
    char *out = tool_args_compact("{\"command\": \"ls -la\", \"cwd\": \"/tmp\"}", 512);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "command=ls -la, cwd=/tmp");
    free(out);
}
END_TEST

START_TEST(test_compact_numbers_and_nested)
{
    char *out = tool_args_compact(
        "{\"line_start\": 10, \"path\": \"main.c\", \"flags\": {\"rec\": true}}",
        512);
    ck_assert_ptr_nonnull(out);
    /* non-strings render as compact JSON; strings stay bare */
    ck_assert_str_eq(out, "line_start=10, path=main.c, flags={\"rec\":true}");
    free(out);
}
END_TEST

START_TEST(test_compact_empty_object_yields_empty)
{
    char *out = tool_args_compact("{}", 512);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "");
    free(out);
}
END_TEST

START_TEST(test_compact_null_and_garbage_input)
{
    char *out = tool_args_compact(NULL, 512);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "");
    free(out);

    /* unparseable input degrades to the raw text, bounded */
    out = tool_args_compact("not json at all", 512);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "not json at all");
    free(out);

    /* arrays are not objects: raw fallback */
    out = tool_args_compact("[1, 2]", 512);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "[1, 2]");
    free(out);
}
END_TEST

START_TEST(test_compact_truncates_at_codepoint_boundary)
{
    /* the CJK value 中文 is 6 bytes: a 10-byte cap keeps "name=" + the
     * first char whole and cuts at the boundary — never mid-sequence */
    char *out = tool_args_compact("{\"name\": \"\xE4\xB8\xAD\xE6\x96\x87\"}", 10);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "name=\xE4\xB8\xAD");
    free(out);

    /* plain ASCII truncation, no marker room left */
    out = tool_args_compact("{\"name\": \"abcdef\"}", 10);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "name=abcd");
    free(out);

    /* a 4-byte codepoint (emoji) leaves marker room: the "…" appears.
     * "name=" (5) + emoji (4) + emoji (4) + emoji (4) = 17 bytes; a
     * 13-byte cap keeps "name=" + one emoji (9) and the marker (3) */
    out = tool_args_compact(
        "{\"name\": \"\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\"}", 13);
    ck_assert_ptr_nonnull(out);
    ck_assert_str_eq(out, "name=\xF0\x9F\x98\x80\xE2\x80\xA6");
    free(out);

    /* tiny cap: no marker fits, the result stays within the cap */
    out = tool_args_compact("{\"name\": \"abc\"}", 3);
    ck_assert_ptr_nonnull(out);
    ck_assert_int_le(strlen(out), 3);
    free(out);
}
END_TEST

START_TEST(test_compact_tiny_cap_is_safe)
{
    /* max_len is clamped to >= 1; the result must stay within it */
    char *out = tool_args_compact("{\"name\": \"abcdefgh\"}", 1);
    ck_assert_ptr_nonnull(out);
    ck_assert_int_le(strlen(out), 1);
    free(out);
}
END_TEST

static Suite *suite(void)
{
    Suite *s = suite_create("tool_args");
    TCase *tc = tcase_create("compact");
    tcase_add_test(tc, test_compact_object_strings_bare);
    tcase_add_test(tc, test_compact_numbers_and_nested);
    tcase_add_test(tc, test_compact_empty_object_yields_empty);
    tcase_add_test(tc, test_compact_null_and_garbage_input);
    tcase_add_test(tc, test_compact_truncates_at_codepoint_boundary);
    tcase_add_test(tc, test_compact_tiny_cap_is_safe);
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
