#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "tools/tool.h"
#include "safety/safety.h"
#include "utils/string_utils.h"

/* test_grep_tool - grep tool unit tests. Depends on: check, the module under test. */
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

/* L4 regression: >32 KB of matches across two files used to push the
 * accumulator past the 32768-byte stack buffer (snprintf returns the
 * would-be length), so the second file computed cap - *pos as a size_t
 * underflow and wrote past the buffer. The fix clamps *pos to cap and
 * stops at the first truncated line; old code dies in ASan with a
 * stack-buffer-overflow. */
static void write_match_file(const char *path, int lines)
{
    FILE *file = fopen(path, "w");
    ck_assert_ptr_nonnull(file);
    for (int i = 0; i < lines; i++)
        ck_assert_int_ge(fputs(
            "MATCH xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n",
            file), 0);
    ck_assert_int_eq(fclose(file), 0);
}

START_TEST(test_grep_large_output_across_files_stays_in_bounds)
{
    char big1[512];
    char big2[512];
    ck_assert_int_lt(snprintf(big1, sizeof(big1), "%s/big1.txt", workspace),
                     (int)sizeof(big1));
    ck_assert_int_lt(snprintf(big2, sizeof(big2), "%s/big2.txt", workspace),
                     (int)sizeof(big2));
    write_match_file(big1, 360);
    write_match_file(big2, 360);

    SafetyConfig *safety = safety_config_create();
    safety->workspace = str_dup(workspace);
    Tool *tool = tool_grep_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(
        tool, "{\"pattern\":\"MATCH\",\"path\":\".\"}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_null(result->error);
    /* Truncation must still yield a usable, terminated result */
    ck_assert_int_gt(strlen(result->content), 0);
    ck_assert_int_le(strlen(result->content), 32768);

    tool_result_free(result);
    tool->destroy(tool);
    safety_config_free(safety);
    ck_assert_int_eq(unlink(big1), 0);
    ck_assert_int_eq(unlink(big2), 0);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("GrepTool");
    TCase *tc = tcase_create("Confinement");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_grep_skips_symlinks_outside_workspace);
    tcase_add_test(tc, test_grep_large_output_across_files_stays_in_bounds);
    tcase_set_timeout(tc, 30);
    suite_add_tcase(suite, tc);
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
