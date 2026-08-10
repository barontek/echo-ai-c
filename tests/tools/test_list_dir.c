/*
 * test_list_dir.c - unit tests for the list_dir tool: directory
 * listing, dotfile skipping, empty dirs, missing dirs. Depends on:
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

Tool *tool_list_dir_create(SafetyConfig *safety);

START_TEST(test_list_dir_lists_entries_with_dir_suffix)
{
    char ws[] = "/tmp/echo_ld_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    FILE *f = fopen("/tmp/echo_ld_file_probe", "w");
    if (f) fclose(f);
    char file_path[512];
    ck_assert_int_lt(snprintf(file_path, sizeof(file_path), "%s/file.txt", ws),
                     (int)sizeof(file_path));
    f = fopen(file_path, "w");
    ck_assert_ptr_nonnull(f);
    fclose(f);
    char sub_path[512];
    ck_assert_int_lt(snprintf(sub_path, sizeof(sub_path), "%s/sub", ws),
                     (int)sizeof(sub_path));
    ck_assert_int_eq(mkdir(sub_path, 0755), 0);
    char hidden_path[512];
    ck_assert_int_lt(snprintf(hidden_path, sizeof(hidden_path), "%s/.hidden", ws),
                     (int)sizeof(hidden_path));
    f = fopen(hidden_path, "w");
    if (f) fclose(f);
    (void)file_path; (void)sub_path; (void)hidden_path;

    SafetyConfig *safety = safety_config_create();
    ck_assert_ptr_nonnull(safety);
    safety->workspace = str_dup(ws);
    Tool *tool = tool_list_dir_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"path\":\".\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert(strstr(r->content, "file.txt\n") != NULL);
    ck_assert(strstr(r->content, "sub/\n") != NULL);
    ck_assert(strstr(r->content, ".hidden") == NULL);
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_list_dir_empty_directory_reports_placeholder)
{
    char ws[] = "/tmp/echo_ld_empty_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    SafetyConfig *safety = safety_config_create();
    ck_assert_ptr_nonnull(safety);
    safety->workspace = str_dup(ws);
    Tool *tool = tool_list_dir_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"path\":\".\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content, "(empty directory)");
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    ck_assert_int_eq(rmdir(ws), 0);
}
END_TEST

START_TEST(test_list_dir_missing_directory_is_file_not_found)
{
    char ws[] = "/tmp/echo_ld_missing_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    SafetyConfig *safety = safety_config_create();
    ck_assert_ptr_nonnull(safety);
    safety->workspace = str_dup(ws);
    Tool *tool = tool_list_dir_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"path\":\"nope\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "file_not_found");
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    ck_assert_int_eq(rmdir(ws), 0);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("ListDir");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_list_dir_lists_entries_with_dir_suffix);
    tcase_add_test(tc, test_list_dir_empty_directory_reports_placeholder);
    tcase_add_test(tc, test_list_dir_missing_directory_is_file_not_found);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
