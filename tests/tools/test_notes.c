#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "tools/tool.h"
#include "safety/safety.h"

/* test_notes - notes tool unit tests. Depends on: check, the module under test. */
Tool *tool_notes_create(SafetyConfig *safety);
void notes_test_set_alloc_fail(int nth_allocation);

static char home[64];

static void create_notes_parent(const char *home_dir)
{
    char path[512];
    ck_assert_int_lt(snprintf(path, sizeof(path), "%s/.config", home_dir),
                     (int)sizeof(path));
    ck_assert_int_eq(mkdir(path, 0700), 0);
    ck_assert_int_lt(snprintf(path, sizeof(path), "%s/.config/echo-ai", home_dir),
                     (int)sizeof(path));
    ck_assert_int_eq(mkdir(path, 0700), 0);
}

static void setup(void)
{
    snprintf(home, sizeof(home), "/tmp/echo_notes_XXXXXX");
    ck_assert_ptr_nonnull(mkdtemp(home));
    create_notes_parent(home);
    ck_assert_int_eq(setenv("HOME", home, 1), 0);
}

static void teardown(void)
{
    /* E11: the fault-injection tests leak their mkdtemp trees and leave
     * HOME pointing at a deleted directory — remove the tree and restore
     * the environment so serial-mode runs are order-independent. */
    char rm[512];
    ck_assert_int_lt(snprintf(rm, sizeof(rm), "rm -rf %s", home),
                     (int)sizeof(rm));
    int rc = system(rm);
    (void)rc;
    unsetenv("HOME");
}

START_TEST(test_notes_rejects_traversal_name)
{

    SafetyConfig *safety = safety_config_create();
    Tool *tool = tool_notes_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(
        tool, "{\"action\":\"write\",\"name\":\"../outside\",\"content\":\"bad\"}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_nonnull(result->error);
    tool_result_free(result);

    char outside[512];
    ck_assert_int_lt(snprintf(outside, sizeof(outside),
                             "%s/.config/echo-ai/outside.md", home),
                     (int)sizeof(outside));
    ck_assert_int_ne(access(outside, F_OK), 0);

    tool->destroy(tool);
    safety_config_free(safety);
    char command[600];
    ck_assert_int_lt(snprintf(command, sizeof(command), "rm -rf %s", home),
                     (int)sizeof(command));
    int cleanup_rc = system(command);
    ck_assert_int_eq(cleanup_rc, 0);
}
END_TEST

START_TEST(test_notes_normal_name_round_trips)
{
    SafetyConfig *safety = safety_config_create();
    Tool *tool = tool_notes_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *write_result = tool->execute(
        tool, "{\"action\":\"write\",\"name\":\"meeting-2026\",\"content\":\"hello\"}");
    ck_assert_ptr_nonnull(write_result);
    ck_assert_ptr_null(write_result->error);
    tool_result_free(write_result);

    ToolResult *read_result = tool->execute(
        tool, "{\"action\":\"read\",\"name\":\"meeting-2026\"}");
    ck_assert_ptr_nonnull(read_result);
    ck_assert_ptr_null(read_result->error);
    ck_assert_str_eq(read_result->content, "hello");
    tool_result_free(read_result);

    tool->destroy(tool);
    safety_config_free(safety);
    char command[600];
    ck_assert_int_lt(snprintf(command, sizeof(command), "rm -rf %s", home),
                     (int)sizeof(command));
    int cleanup_rc = system(command);
    ck_assert_int_eq(cleanup_rc, 0);
}
END_TEST

/* tool_notes_create allocates: calloc(t), calloc(ctx), 3x str_dup; a
 * partial tool must never be returned (NULL with nothing leaked). */
START_TEST(test_notes_create_allocation_failure_returns_null)
{
    for (int fail_at = 1; fail_at <= 3; fail_at++)
    {
        notes_test_set_alloc_fail(fail_at);
        SafetyConfig *safety = safety_config_create();
        Tool *tool = tool_notes_create(safety);
        ck_assert_ptr_null(tool);
        safety_config_free(safety);
    }
    notes_test_set_alloc_fail(-1);
}
END_TEST

/* notes_execute's write path allocates action, name, content, then the
 * file path before writing; every failure must return an error without
 * creating the note and without leaking (ASan-verified). */
START_TEST(test_notes_write_allocation_failure_leaves_no_note)
{
    SafetyConfig *safety = safety_config_create();
    Tool *tool = tool_notes_create(safety);
    ck_assert_ptr_nonnull(tool);

    for (int fail_at = 1; fail_at <= 4; fail_at++)
    {
        notes_test_set_alloc_fail(fail_at);
        ToolResult *result = tool->execute(
            tool, "{\"action\":\"write\",\"name\":\"fault\",\"content\":\"boom\"}");
        ck_assert_ptr_nonnull(result);
        ck_assert_ptr_nonnull(result->error);
        tool_result_free(result);
    }
    notes_test_set_alloc_fail(-1);

    char path[512];
    ck_assert_int_lt(snprintf(path, sizeof(path),
                             "%s/.config/echo-ai/notes/fault.md", home),
                     (int)sizeof(path));
    ck_assert_int_ne(access(path, F_OK), 0);

    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

/* After fault injection the hook must reset: the same write succeeds. */
START_TEST(test_notes_write_succeeds_after_fault_injection_reset)
{
    SafetyConfig *safety = safety_config_create();
    Tool *tool = tool_notes_create(safety);
    ck_assert_ptr_nonnull(tool);

    notes_test_set_alloc_fail(1);
    ToolResult *failed = tool->execute(
        tool, "{\"action\":\"write\",\"name\":\"ok\",\"content\":\"data\"}");
    ck_assert_ptr_nonnull(failed);
    ck_assert_ptr_nonnull(failed->error);
    tool_result_free(failed);

    notes_test_set_alloc_fail(-1);
    ToolResult *result = tool->execute(
        tool, "{\"action\":\"write\",\"name\":\"ok\",\"content\":\"data\"}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_null(result->error);
    tool_result_free(result);

    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("Notes");
    TCase *tc = tcase_create("Paths");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_notes_rejects_traversal_name);
    tcase_add_test(tc, test_notes_normal_name_round_trips);
    tcase_set_timeout(tc, 30);
    suite_add_tcase(suite, tc);

    TCase *tc_fault = tcase_create("FaultInjection");
    tcase_add_checked_fixture(tc_fault, setup, teardown);
    tcase_set_timeout(tc_fault, 30);
    tcase_add_test(tc_fault, test_notes_create_allocation_failure_returns_null);
    tcase_add_test(tc_fault, test_notes_write_allocation_failure_leaves_no_note);
    tcase_add_test(tc_fault, test_notes_write_succeeds_after_fault_injection_reset);
    suite_add_tcase(suite, tc_fault);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
