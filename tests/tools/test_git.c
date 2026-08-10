/*
 * test_git.c - unit tests for the git tool against a throwaway repo
 * created in setup (the tool runs git in the process CWD; Check fork
 * mode keeps the chdir per-test). Skipped when git is unavailable.
 * Depends on: check, tool.h.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "tools/tool.h"
#include "safety/safety.h"

Tool *tool_git_create(SafetyConfig *safety);

static char repo_dir[64];
static char old_cwd[1024];

static void git_setup(void)
{
    if (system("command -v git >/dev/null 2>&1") != 0) return;
    snprintf(repo_dir, sizeof(repo_dir), "/tmp/echo_git_XXXXXX");
    ck_assert_ptr_nonnull(mkdtemp(repo_dir));
    ck_assert_ptr_nonnull(getcwd(old_cwd, sizeof(old_cwd)));
    ck_assert_int_eq(chdir(repo_dir), 0);
    ck_assert_int_eq(system("git init -q"), 0);
    ck_assert_int_eq(system("git config user.email t@t.t"), 0);
    ck_assert_int_eq(system("git config user.name tester"), 0);
    FILE *f = fopen("file.txt", "w");
    ck_assert_ptr_nonnull(f);
    fputs("content", f);
    fclose(f);
    ck_assert_int_eq(system("git add file.txt && git commit -qm initial"), 0);
}

static void git_teardown(void)
{
    if (!repo_dir[0]) return;
    chdir(old_cwd);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", repo_dir);
    ck_assert_int_eq(system(cmd), 0);
    repo_dir[0] = '\0';
}

START_TEST(test_git_status_reports_clean)
{
    Tool *tool = tool_git_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"operation\":\"status\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    /* git status --short prints nothing on a clean tree */
    ck_assert_str_eq(r->content, "");
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_git_log_shows_commit)
{
    Tool *tool = tool_git_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"operation\":\"log\",\"count\":5}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert(strstr(r->content, "initial") != NULL);
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_git_diff_shows_uncommitted_change)
{
    FILE *f = fopen("file.txt", "a");
    ck_assert_ptr_nonnull(f);
    fputs("\nmore", f);
    fclose(f);
    Tool *tool = tool_git_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"operation\":\"diff\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert(strstr(r->content, "+more") != NULL);
    tool_result_free(r);
    tool->destroy(tool);
    f = fopen("file.txt", "w");
    ck_assert_ptr_nonnull(f);
    fputs("content", f);
    fclose(f);
}
END_TEST

START_TEST(test_git_unknown_operation_is_validation_error)
{
    Tool *tool = tool_git_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"operation\":\"frobnicate\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("GitTool");
    TCase *tc = tcase_create("Execute");
    tcase_add_checked_fixture(tc, git_setup, git_teardown);
    tcase_set_timeout(tc, 60);
    tcase_add_test(tc, test_git_status_reports_clean);
    tcase_add_test(tc, test_git_log_shows_commit);
    tcase_add_test(tc, test_git_diff_shows_uncommitted_change);
    tcase_add_test(tc, test_git_unknown_operation_is_validation_error);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
