/*
 * test_glob_tool.c - unit tests for the glob tool: pattern matching
 * under the workspace root, no-match output, and safety rejection.
 * Depends on: check, tool.h, safety, config.
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

Tool *tool_glob_create(SafetyConfig *safety);

static SafetyConfig *make_safety(const char *workspace)
{
    SafetyConfig *safety = safety_config_create();
    ck_assert_ptr_nonnull(safety);
    safety->workspace = str_dup(workspace);
    return safety;
}

START_TEST(test_glob_matches_files_under_workspace)
{
    char ws[] = "/tmp/echo_glob_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char p1[512], p2[512];
    ck_assert_int_lt(snprintf(p1, sizeof(p1), "%s/a.txt", ws),
                     (int)sizeof(p1));
    ck_assert_int_lt(snprintf(p2, sizeof(p2), "%s/b.md", ws),
                     (int)sizeof(p2));
    FILE *f = fopen(p1, "w");
    ck_assert_ptr_nonnull(f);
    fclose(f);
    f = fopen(p2, "w");
    ck_assert_ptr_nonnull(f);
    fclose(f);

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_glob_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"pattern\":\"*.txt\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert(strstr(r->content, "a.txt\n") != NULL);
    ck_assert(strstr(r->content, "b.md") == NULL);
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_glob_no_match_reports_placeholder)
{
    char ws[] = "/tmp/echo_glob_nomatch_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_glob_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"pattern\":\"*.zzz\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content, "(no matches)");
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    ck_assert_int_eq(rmdir(ws), 0);
}
END_TEST

START_TEST(test_glob_missing_pattern_is_validation_error)
{
    char ws[] = "/tmp/echo_glob_val_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_glob_create(safety);
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
    Suite *suite = suite_create("GlobTool");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_glob_matches_files_under_workspace);
    tcase_add_test(tc, test_glob_no_match_reports_placeholder);
    tcase_add_test(tc, test_glob_missing_pattern_is_validation_error);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
