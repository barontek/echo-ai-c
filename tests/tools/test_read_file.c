/*
 * test_read_file.c - unit tests for the read_file tool: content reads,
 * size caps, missing files, and safety-policy rejection. Depends on:
 * check, tool.h, safety, config.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "utils/string_utils.h"
#include "tools/tool.h"
#include "safety/safety.h"

Tool *tool_read_file_create(SafetyConfig *safety);

static SafetyConfig *make_safety(const char *workspace)
{
    SafetyConfig *safety = safety_config_create();
    ck_assert_ptr_nonnull(safety);
    safety->workspace = str_dup(workspace);
    safety->max_file_size = 4096;
    return safety;
}

START_TEST(test_read_file_returns_content)
{
    char ws[] = "/tmp/echo_rf_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/a.txt", ws),
                     (int)sizeof(abs_path));
    FILE *f = fopen(abs_path, "w");
    ck_assert_ptr_nonnull(f);
    fputs("hello file", f);
    fclose(f);

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_read_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"file_path\":\"a.txt\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content, "hello file");
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_read_file_missing_file_is_file_not_found)
{
    char ws[] = "/tmp/echo_rf_missing_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_read_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"file_path\":\"nope.txt\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "file_not_found");
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    ck_assert_int_eq(rmdir(ws), 0);
}
END_TEST

START_TEST(test_read_file_oversized_is_policy_denied)
{
    char ws[] = "/tmp/echo_rf_big_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/big.txt", ws),
                     (int)sizeof(abs_path));
    FILE *f = fopen(abs_path, "w");
    ck_assert_ptr_nonnull(f);
    for (int i = 0; i < 100; i++) fputs("0123456789abcdef", f);
    fclose(f);

    SafetyConfig *safety = make_safety(ws);
    safety->max_file_size = 100;
    Tool *tool = tool_read_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"file_path\":\"big.txt\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "policy_denied");
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_read_file_missing_arg_is_validation_error)
{
    char ws[] = "/tmp/echo_rf_val_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_read_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    ck_assert_int_eq(rmdir(ws), 0);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("ReadFile");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_read_file_returns_content);
    tcase_add_test(tc, test_read_file_missing_file_is_file_not_found);
    tcase_add_test(tc, test_read_file_oversized_is_policy_denied);
    tcase_add_test(tc, test_read_file_missing_arg_is_validation_error);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
