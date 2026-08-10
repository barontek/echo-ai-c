/*
 * test_python_execute.c - unit tests for the python_execute tool
 * (real subprocess; skipped when python3 is unavailable). Depends on:
 * check, tool.h, safety.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "tools/tool.h"
#include "safety/safety.h"

Tool *tool_python_execute_create(SafetyConfig *safety);

static int python_available(void)
{
    static int checked = 0;
    static int available = 0;
    if (!checked)
    {
        available = system("command -v python3 >/dev/null 2>&1") == 0;
        checked = 1;
    }
    return available;
}

START_TEST(test_python_execute_prints_stdout)
{
    if (!python_available()) return;
    Tool *tool = tool_python_execute_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"code\":\"print('hello from python')\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert(strstr(r->content, "hello from python") != NULL);
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_python_execute_nonzero_exit_reports_error)
{
    if (!python_available()) return;
    Tool *tool = tool_python_execute_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"code\":\"import sys; sys.exit(3)\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_python_execute_missing_code_is_validation_error)
{
    Tool *tool = tool_python_execute_create(NULL);
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
    Suite *suite = suite_create("PythonExecute");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 60);
    tcase_add_test(tc, test_python_execute_prints_stdout);
    tcase_add_test(tc, test_python_execute_nonzero_exit_reports_error);
    tcase_add_test(tc, test_python_execute_missing_code_is_validation_error);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
