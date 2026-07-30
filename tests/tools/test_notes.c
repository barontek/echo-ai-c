#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "tools/tool.h"
#include "safety/safety.h"

Tool *tool_notes_create(SafetyConfig *safety);

static void create_notes_parent(const char *home)
{
    char path[512];
    ck_assert_int_lt(snprintf(path, sizeof(path), "%s/.config", home),
                     (int)sizeof(path));
    ck_assert_int_eq(mkdir(path, 0700), 0);
    ck_assert_int_lt(snprintf(path, sizeof(path), "%s/.config/echo-ai", home),
                     (int)sizeof(path));
    ck_assert_int_eq(mkdir(path, 0700), 0);
}

START_TEST(test_notes_rejects_traversal_name)
{
    char home[] = "/tmp/echo_notes_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(home));
    create_notes_parent(home);
    ck_assert_int_eq(setenv("HOME", home, 1), 0);

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
    char home[] = "/tmp/echo_notes_roundtrip_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(home));
    create_notes_parent(home);
    ck_assert_int_eq(setenv("HOME", home, 1), 0);

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

int main(void)
{
    Suite *suite = suite_create("Notes");
    TCase *tc = tcase_create("Paths");
    tcase_add_test(tc, test_notes_rejects_traversal_name);
    tcase_add_test(tc, test_notes_normal_name_round_trips);
    suite_add_tcase(suite, tc);
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
