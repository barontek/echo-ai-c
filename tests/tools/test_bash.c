#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include "tools/tool.h"
#include "safety/safety.h"

Tool *tool_bash_create(SafetyConfig *safety);

START_TEST(test_bash_drains_large_output_without_timeout)
{
    SafetyConfig *safety = safety_config_create();
    safety->max_execution_time = 5;
    Tool *tool = tool_bash_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(
        tool, "{\"command\":\"yes x | head -c 100000\"}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_null(result->error);
    ck_assert_ptr_nonnull(strstr(result->content, "Exit code: 0"));
    tool_result_free(result);
    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

START_TEST(test_bash_terminates_background_descendants)
{
    SafetyConfig *safety = safety_config_create();
    safety->max_execution_time = 5;
    Tool *tool = tool_bash_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(
        tool, "{\"command\":\"(trap '' TERM; while :; do sleep 1; done) >/dev/null 2>&1 & echo $!\"}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_null(result->error);
    const char *pid_text = strchr(result->content, '\n');
    ck_assert_ptr_nonnull(pid_text);
    long child_pid = strtol(pid_text + 1, NULL, 10);
    ck_assert_int_gt(child_pid, 0);
    for (int i = 0; i < 20 && kill((pid_t)child_pid, 0) == 0; i++)
        usleep(50000);
    ck_assert_int_eq(kill((pid_t)child_pid, 0), -1);
    ck_assert_int_eq(errno, ESRCH);
    tool_result_free(result);
    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

START_TEST(test_bash_timeout_is_bounded)
{
    SafetyConfig *safety = safety_config_create();
    safety->max_execution_time = 1;
    Tool *tool = tool_bash_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(tool, "{\"command\":\"sleep 5\"}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_nonnull(result->error);
    ck_assert_str_eq(result->error_category, "timeout");
    tool_result_free(result);
    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("Bash");
    TCase *tc = tcase_create("ProcessIO");
    tcase_set_timeout(tc, 10);
    tcase_add_test(tc, test_bash_drains_large_output_without_timeout);
    tcase_add_test(tc, test_bash_timeout_is_bounded);
    tcase_add_test(tc, test_bash_terminates_background_descendants);
    suite_add_tcase(suite, tc);
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
