#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include "tools/tool.h"
#include "safety/safety.h"

/* test_bash - shell tool unit tests. Depends on: check, the module under test. */
Tool *tool_bash_create(SafetyConfig *safety);

static int process_is_running(pid_t pid)
{
    if (kill(pid, 0) != 0) return errno != ESRCH;
#ifdef __linux__
    char stat_path[64];
    if (snprintf(stat_path, sizeof(stat_path), "/proc/%ld/stat", (long)pid) < 0)
        return 1;
    FILE *stat_file = fopen(stat_path, "r");
    if (!stat_file) return 1;
    char stat_line[512];
    char *line = fgets(stat_line, sizeof(stat_line), stat_file);
    int close_rc = fclose(stat_file);
    if (!line || close_rc != 0) return 1;
    char *command_end = strrchr(stat_line, ')');
    if (!command_end || command_end[1] != ' ') return 1;
    return command_end[2] != 'Z';
#else
    return 1;
#endif
}

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
    for (int i = 0; i < 20 && process_is_running((pid_t)child_pid); i++)
        usleep(50000);
    ck_assert_int_eq(process_is_running((pid_t)child_pid), 0);
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

START_TEST(test_bash_timeout_override_applied)
{
    /* T4: a per-call timeout shorter than the policy maximum must be
     * honored. On pre-T4 code the override is ignored and the policy
     * default (30s) applies, so this test times out and fails. */
    SafetyConfig *safety = safety_config_create();
    safety->max_execution_time = 30;
    Tool *tool = tool_bash_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(
        tool, "{\"command\":\"sleep 5\",\"timeout\":1}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_nonnull(result->error);
    ck_assert_str_eq(result->error_category, "timeout");
    tool_result_free(result);
    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

START_TEST(test_bash_timeout_zero_rejected)
{
    SafetyConfig *safety = safety_config_create();
    Tool *tool = tool_bash_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(tool, "{\"command\":\"echo hi\",\"timeout\":0}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_nonnull(result->error);
    ck_assert_str_eq(result->error_category, "validation_error");
    tool_result_free(result);
    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

START_TEST(test_bash_timeout_fractional_rejected)
{
    /* T4: non-integral timeouts would silently truncate to a shorter
     * (possibly zero → 30s default) limit; reject them outright. */
    SafetyConfig *safety = safety_config_create();
    Tool *tool = tool_bash_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(tool, "{\"command\":\"echo hi\",\"timeout\":0.5}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_nonnull(result->error);
    ck_assert_str_eq(result->error_category, "validation_error");
    tool_result_free(result);
    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

START_TEST(test_bash_timeout_above_policy_capped)
{
    /* T4: requesting more than the policy maximum must be clamped to
     * the policy limit — the tcase timeout (10s) would trip if the
     * 9999s request were honored instead. */
    SafetyConfig *safety = safety_config_create();
    safety->max_execution_time = 2;
    Tool *tool = tool_bash_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(
        tool, "{\"command\":\"sleep 10\",\"timeout\":9999}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_nonnull(result->error);
    ck_assert_str_eq(result->error_category, "timeout");
    tool_result_free(result);
    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

START_TEST(test_bash_large_output_spills_to_temp)
{
    /* T4a: output beyond the tail budget must end with a full-output
     * pointer, and the temp file must hold ALL the output. */
    SafetyConfig *safety = safety_config_create();
    safety->max_execution_time = 5;
    Tool *tool = tool_bash_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(
        tool, "{\"command\":\"seq 1 10000\"}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_null(result->error);
    ck_assert_ptr_nonnull(strstr(result->content, "Exit code: 0"));

    const char *marker = strstr(result->content, "[Full output: ");
    ck_assert_ptr_nonnull(marker);
    const char *path_start = marker + strlen("[Full output: ");
    const char *path_end = strchr(path_start, ']');
    ck_assert_ptr_nonnull(path_end);
    char *spill_path = malloc((size_t)(path_end - path_start) + 1);
    ck_assert_ptr_nonnull(spill_path);
    memcpy(spill_path, path_start, (size_t)(path_end - path_start));
    spill_path[path_end - path_start] = '\0';

    /* The result shows the tail (last lines), not the head. */
    ck_assert_ptr_nonnull(strstr(result->content, "9999\n10000"));
    ck_assert_ptr_null(strstr(result->content, "1\n2\n3\n4\n5"));

    /* The spill file holds the complete output. */
    FILE *f = fopen(spill_path, "r");
    ck_assert_ptr_nonnull(f);
    char buf[65536];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    ck_assert_ptr_nonnull(strstr(buf, "1\n2\n3\n"));
    ck_assert_ptr_nonnull(strstr(buf, "10000"));
    unlink(spill_path);
    free(spill_path);

    tool_result_free(result);
    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

START_TEST(test_bash_small_output_no_spill)
{
    /* T4a regression: small output keeps the plain format. */
    SafetyConfig *safety = safety_config_create();
    safety->max_execution_time = 5;
    Tool *tool = tool_bash_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(tool, "{\"command\":\"echo tiny\"}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_null(result->error);
    ck_assert_ptr_null(strstr(result->content, "[Full output: "));
    ck_assert_ptr_nonnull(strstr(result->content, "tiny"));
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
    tcase_add_test(tc, test_bash_timeout_override_applied);
    tcase_add_test(tc, test_bash_timeout_zero_rejected);
    tcase_add_test(tc, test_bash_timeout_fractional_rejected);
    tcase_add_test(tc, test_bash_timeout_above_policy_capped);
    tcase_add_test(tc, test_bash_large_output_spills_to_temp);
    tcase_add_test(tc, test_bash_small_output_no_spill);
    tcase_add_test(tc, test_bash_terminates_background_descendants);
    suite_add_tcase(suite, tc);
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
