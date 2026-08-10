/*
 * test_tool_humanizer.c - unit tests for the humanizer tool: paragraph,
 * bullet, and summary styles, plus validation errors. Depends on: check,
 * tool.h.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "tools/tool.h"
#include "safety/safety.h"

Tool *tool_humanizer_create(SafetyConfig *safety);

START_TEST(test_humanizer_paragraph_style_passes_text_through)
{
    Tool *tool = tool_humanizer_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(
        tool, "{\"content\":\"plain text\",\"style\":\"paragraph\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content, "plain text");
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_humanizer_bullet_style_formats_json_array)
{
    Tool *tool = tool_humanizer_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(
        tool, "{\"content\":\"[\\\"one\\\",\\\"two\\\"]\",\"style\":\"bullet\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content, "- one\n- two\n");
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_humanizer_bullet_style_non_array_passes_text_through)
{
    Tool *tool = tool_humanizer_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(
        tool, "{\"content\":\"not json\",\"style\":\"bullet\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content, "not json");
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_humanizer_summary_style_truncates_long_text)
{
    Tool *tool = tool_humanizer_create(NULL);
    ck_assert_ptr_nonnull(tool);
    char long_text[700];
    memset(long_text, 'x', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    char args[800];
    ck_assert_int_lt(snprintf(args, sizeof(args),
                              "{\"content\":\"%s\",\"style\":\"summary\"}",
                              long_text), (int)sizeof(args));
    ToolResult *r = tool->execute(tool, args);
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_int_eq(strlen(r->content), 503); /* 500 + "..." */
    ck_assert_str_eq(r->content + 500, "...");
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_humanizer_summary_short_text_unchanged)
{
    Tool *tool = tool_humanizer_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(
        tool, "{\"content\":\"short\",\"style\":\"summary\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content, "short");
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_humanizer_missing_content_is_validation_error)
{
    Tool *tool = tool_humanizer_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("Humanizer");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_humanizer_paragraph_style_passes_text_through);
    tcase_add_test(tc, test_humanizer_bullet_style_formats_json_array);
    tcase_add_test(tc, test_humanizer_bullet_style_non_array_passes_text_through);
    tcase_add_test(tc, test_humanizer_summary_style_truncates_long_text);
    tcase_add_test(tc, test_humanizer_summary_short_text_unchanged);
    tcase_add_test(tc, test_humanizer_missing_content_is_validation_error);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
