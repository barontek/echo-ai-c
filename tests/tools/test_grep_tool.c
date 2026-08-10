#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "tools/tool.h"
#include "safety/safety.h"
#include "utils/string_utils.h"

Tool *tool_grep_create(SafetyConfig *safety);

static char workspace[64];
static char outside_dir[64];
static char outside_file[512];
static char link_path[512];

static void setup(void)
{
    snprintf(workspace, sizeof(workspace), "/tmp/echo_grep_ws_XXXXXX");
    snprintf(outside_dir, sizeof(outside_dir), "/tmp/echo_grep_out_XXXXXX");
    ck_assert_ptr_nonnull(mkdtemp(workspace));
    ck_assert_ptr_nonnull(mkdtemp(outside_dir));

    ck_assert_int_lt(snprintf(outside_file, sizeof(outside_file),
                              "%s/secret.txt", outside_dir),
                     (int)sizeof(outside_file));
    FILE *file = fopen(outside_file, "w");
    ck_assert_ptr_nonnull(file);
    ck_assert_int_gt(fputs("TOP_SECRET_VALUE\n", file), 0);
    ck_assert_int_eq(fclose(file), 0);

    ck_assert_int_lt(snprintf(link_path, sizeof(link_path), "%s/escape", workspace),
                     (int)sizeof(link_path));
    ck_assert_int_eq(symlink(outside_dir, link_path), 0);
}

static void teardown(void)
{
    ck_assert_int_eq(unlink(link_path), 0);
    ck_assert_int_eq(unlink(outside_file), 0);
    ck_assert_int_eq(rmdir(outside_dir), 0);
    ck_assert_int_eq(rmdir(workspace), 0);
}

START_TEST(test_grep_skips_symlinks_outside_workspace)
{
    SafetyConfig *safety = safety_config_create();
    safety->workspace = str_dup(workspace);
    Tool *tool = tool_grep_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(
        tool, "{\"pattern\":\"TOP_SECRET_VALUE\",\"path\":\".\"}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_null(result->error);
    ck_assert_ptr_null(strstr(result->content, "TOP_SECRET_VALUE"));

    tool_result_free(result);
    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("GrepTool");
    TCase *tc = tcase_create("Confinement");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_grep_skips_symlinks_outside_workspace);
    tcase_set_timeout(tc, 30);
    suite_add_tcase(suite, tc);
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
