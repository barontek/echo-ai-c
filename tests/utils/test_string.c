#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "utils/string_utils.h"

/* test_string - unit tests for string. Depends on: check, the module under test. */
START_TEST(test_str_trim_removes_leading_and_trailing_whitespace)
{
    char s1[] = "  hello  ";
    ck_assert_str_eq(str_trim(s1), "hello");

    char s2[] = "no trim";
    ck_assert_str_eq(str_trim(s2), "no trim");

    char s3[] = "";
    ck_assert_str_eq(str_trim(s3), "");

    char s4[] = "   ";
    ck_assert_str_eq(str_trim(s4), "");
}
END_TEST

START_TEST(test_str_starts_with_matches_prefix)
{
    ck_assert_int_eq(str_starts_with("hello world", "hello"), 1);
    ck_assert_int_eq(str_starts_with("hello world", "world"), 0);
    ck_assert_int_eq(str_starts_with("", ""), 1);
}
END_TEST

START_TEST(test_str_ends_with_matches_suffix)
{
    ck_assert_int_eq(str_ends_with("hello.c", ".c"), 1);
    ck_assert_int_eq(str_ends_with("hello.c", ".h"), 0);
}
END_TEST

START_TEST(test_str_split_divides_string_on_delimiter_and_counts_items)
{
    StrArray arr = str_split("a,b,c", ',');
    ck_assert_int_eq(arr.count, 3);
    ck_assert_str_eq(arr.items[0], "a");
    ck_assert_str_eq(arr.items[1], "b");
    ck_assert_str_eq(arr.items[2], "c");
    str_array_free(&arr);

    arr = str_split("", ',');
    ck_assert_int_eq(arr.count, 1);
    ck_assert_str_eq(arr.items[0], "");
    str_array_free(&arr);
}
END_TEST

START_TEST(test_str_split_many_items_triggers_realloc)
{
    char buf[1024] = {0};
    char *p = buf;
    for (int i = 0; i < 20; i++)
    {
        if (i > 0) *p++ = ',';
        *p++ = 'a' + (i % 26);
    }
    *p = '\0';
    StrArray arr = str_split(buf, ',');
    ck_assert_int_eq(arr.count, 20);
    str_array_free(&arr);
}
END_TEST

START_TEST(test_str_dup_null)
{
    ck_assert_ptr_null(str_dup(NULL));
}
END_TEST

START_TEST(test_str_starts_ends_with_null)
{
    ck_assert_int_eq(str_starts_with(NULL, "hello"), 0);
    ck_assert_int_eq(str_starts_with("hello", NULL), 0);
    ck_assert_int_eq(str_starts_with(NULL, NULL), 0);
    ck_assert_int_eq(str_ends_with(NULL, ".c"), 0);
    ck_assert_int_eq(str_ends_with("hello", NULL), 0);
    ck_assert_int_eq(str_ends_with(NULL, NULL), 0);
}
END_TEST

START_TEST(test_str_ends_with_suffix_longer_than_str)
{
    ck_assert_int_eq(str_ends_with("hi", "hello"), 0);
}
END_TEST

START_TEST(test_strlcpy_copies_and_terminates_exact_fit)
{
    char buf[6];
    size_t ret = strlcpy(buf, "hello", sizeof(buf));
    ck_assert_uint_eq(ret, 5);
    ck_assert_str_eq(buf, "hello");
}
END_TEST

START_TEST(test_strlcpy_truncates_and_reports_would_be_length)
{
    char buf[4];
    size_t ret = strlcpy(buf, "hello", sizeof(buf));
    ck_assert_uint_eq(ret, 5);
    ck_assert_str_eq(buf, "hel");
    ck_assert_uint_eq(buf[3], '\0');
}
END_TEST

START_TEST(test_strlcpy_zero_size_does_not_touch_buffer)
{
    char buf[4] = "xyz";
    size_t ret = strlcpy(buf, "hello", 0);
    ck_assert_uint_eq(ret, 5);
    ck_assert_str_eq(buf, "xyz");
}
END_TEST

START_TEST(test_strlcat_appends_within_budget)
{
    char buf[16] = "foo";
    size_t ret = strlcat(buf, "bar", sizeof(buf));
    ck_assert_uint_eq(ret, 6);
    ck_assert_str_eq(buf, "foobar");
}
END_TEST

START_TEST(test_strlcat_truncates_and_reports_would_be_length)
{
    char buf[6] = "foo";
    size_t ret = strlcat(buf, "bar", sizeof(buf));
    ck_assert_uint_eq(ret, 6);
    ck_assert_str_eq(buf, "fooba");
    ck_assert_uint_eq(buf[5], '\0');
}
END_TEST

START_TEST(test_strlcat_full_buffer_is_noop_with_report)
{
    char buf[4] = "foo";
    size_t ret = strlcat(buf, "bar", sizeof(buf));
    ck_assert_uint_eq(ret, 6);
    ck_assert_str_eq(buf, "foo");
}
END_TEST

/* --- sanitize_json_dup --- */

START_TEST(test_sanitize_json_strips_leading_whitespace)
{
    char *r = sanitize_json_dup("  \t\n{\"key\":\"val\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_str_eq(r, "{\"key\":\"val\"}");
    free(r);
}
END_TEST

START_TEST(test_sanitize_json_strips_markdown_json_fence)
{
    char *r = sanitize_json_dup("```json\n{\"key\":\"val\"}\n```");
    ck_assert_ptr_nonnull(r);
    ck_assert_str_eq(r, "{\"key\":\"val\"}```");
    free(r);
}
END_TEST

START_TEST(test_sanitize_json_strips_markdown_fence_no_lang)
{
    char *r = sanitize_json_dup("```\n{\"key\":\"val\"}\n```");
    ck_assert_ptr_nonnull(r);
    ck_assert_str_eq(r, "{\"key\":\"val\"}```");
    free(r);
}
END_TEST

START_TEST(test_sanitize_json_removes_newlines_outside_strings)
{
    char *r = sanitize_json_dup("{\n  \"key\":\n  \"val\"\n}");
    ck_assert_ptr_nonnull(r);
    ck_assert_str_eq(r, "{  \"key\":  \"val\"}");
    free(r);
}
END_TEST

START_TEST(test_sanitize_json_preserves_newlines_in_strings)
{
    char *r = sanitize_json_dup("{\"text\":\"line1\\nline2\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_str_eq(r, "{\"text\":\"line1\\nline2\"}");
    free(r);
}
END_TEST

START_TEST(test_sanitize_json_removes_trailing_comma_before_bracket)
{
    char *r = sanitize_json_dup("{\"a\": 1,}");
    ck_assert_ptr_nonnull(r);
    ck_assert_str_eq(r, "{\"a\": 1}");
    free(r);
}
END_TEST

START_TEST(test_sanitize_json_removes_trailing_comma_before_brace)
{
    char *r = sanitize_json_dup("[\"a\", \"b\",]");
    ck_assert_ptr_nonnull(r);
    ck_assert_str_eq(r, "[\"a\", \"b\"]");
    free(r);
}
END_TEST

START_TEST(test_sanitize_json_preserves_normal_commas)
{
    char *r = sanitize_json_dup("{\"a\":1,\"b\":2}");
    ck_assert_ptr_nonnull(r);
    ck_assert_str_eq(r, "{\"a\":1,\"b\":2}");
    free(r);
}
END_TEST

START_TEST(test_sanitize_json_null)
{
    ck_assert_ptr_null(sanitize_json_dup(NULL));
}
END_TEST

START_TEST(test_sanitize_json_empty)
{
    char *r = sanitize_json_dup("");
    ck_assert_ptr_nonnull(r);
    ck_assert_str_eq(r, "");
    free(r);
}
END_TEST

START_TEST(test_sanitize_json_trims_trailing_whitespace)
{
    char *r = sanitize_json_dup("{\"key\":\"val\"}  \t\n");
    ck_assert_ptr_nonnull(r);
    ck_assert_str_eq(r, "{\"key\":\"val\"}");
    free(r);
}
END_TEST

Suite *string_suite(void)
{
    Suite *s = suite_create("StringUtils");
    TCase *tc = tcase_create("Core");
    tcase_set_timeout(tc, 10);
    tcase_add_test(tc, test_str_trim_removes_leading_and_trailing_whitespace);
    tcase_add_test(tc, test_str_starts_with_matches_prefix);
    tcase_add_test(tc, test_str_ends_with_matches_suffix);
    tcase_add_test(tc, test_str_split_divides_string_on_delimiter_and_counts_items);
    tcase_add_test(tc, test_str_split_many_items_triggers_realloc);
    tcase_add_test(tc, test_str_dup_null);
    tcase_add_test(tc, test_str_starts_ends_with_null);
    tcase_add_test(tc, test_str_ends_with_suffix_longer_than_str);
    tcase_add_test(tc, test_strlcpy_copies_and_terminates_exact_fit);
    tcase_add_test(tc, test_strlcpy_truncates_and_reports_would_be_length);
    tcase_add_test(tc, test_strlcpy_zero_size_does_not_touch_buffer);
    tcase_add_test(tc, test_strlcat_appends_within_budget);
    tcase_add_test(tc, test_strlcat_truncates_and_reports_would_be_length);
    tcase_add_test(tc, test_strlcat_full_buffer_is_noop_with_report);
    suite_add_tcase(s, tc);

    TCase *tc_json = tcase_create("SanitizeJSON");
    tcase_set_timeout(tc_json, 10);
    tcase_add_test(tc_json, test_sanitize_json_strips_leading_whitespace);
    tcase_add_test(tc_json, test_sanitize_json_strips_markdown_json_fence);
    tcase_add_test(tc_json, test_sanitize_json_strips_markdown_fence_no_lang);
    tcase_add_test(tc_json, test_sanitize_json_removes_newlines_outside_strings);
    tcase_add_test(tc_json, test_sanitize_json_preserves_newlines_in_strings);
    tcase_add_test(tc_json, test_sanitize_json_removes_trailing_comma_before_bracket);
    tcase_add_test(tc_json, test_sanitize_json_removes_trailing_comma_before_brace);
    tcase_add_test(tc_json, test_sanitize_json_preserves_normal_commas);
    tcase_add_test(tc_json, test_sanitize_json_null);
    tcase_add_test(tc_json, test_sanitize_json_empty);
    tcase_add_test(tc_json, test_sanitize_json_trims_trailing_whitespace);
    suite_add_tcase(s, tc_json);
    return s;
}

int main(void)
{
    Suite *s = string_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
