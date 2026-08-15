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
void read_file_test_set_fread_fail(int fail);

static const char *SAMPLE_20 = "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\n"
    "l11\nl12\nl13\nl14\nl15\nl16\nl17\nl18\nl19\nl20\n";

static SafetyConfig *make_safety(const char *workspace)
{
    SafetyConfig *safety = safety_config_create();
    ck_assert_ptr_nonnull(safety);
    safety->workspace = str_dup(workspace);
    safety->max_file_size = 4096;
    return safety;
}

static char *make_sample_file(const char *ws, const char *name,
                              const char *content)
{
    char *abs_path = malloc(strlen(ws) + strlen(name) + 2);
    ck_assert_ptr_nonnull(abs_path);
    snprintf(abs_path, strlen(ws) + strlen(name) + 2, "%s/%s", ws, name);
    FILE *f = fopen(abs_path, "w");
    ck_assert_ptr_nonnull(f);
    fputs(content, f);
    fclose(f);
    return abs_path;
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

START_TEST(test_read_file_offset_limit_pagination)
{
    /* T1: offset=6 limit=5 must return exactly lines 6-10 plus a
     * "more lines" hint; the full read must match the original bytes. */
    char ws[] = "/tmp/echo_rf_win_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char *abs_path = make_sample_file(ws, "w.txt", SAMPLE_20);

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_read_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(
        tool, "{\"file_path\":\"w.txt\",\"offset\":6,\"limit\":5}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content,
        "l6\nl7\nl8\nl9\nl10\n\n"
        "[Showing lines 6-10 of 21. Use offset=11 to continue.]");
    tool_result_free(r);

    ToolResult *full = tool->execute(tool, "{\"file_path\":\"w.txt\"}");
    ck_assert_ptr_nonnull(full);
    ck_assert_ptr_null(full->error);
    ck_assert_str_eq(full->content, SAMPLE_20);
    tool_result_free(full);

    free(abs_path);
    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_read_file_truncation_notice_without_limit)
{
    /* T1: without a limit, more than READ_MAX_LINES yields the first
     * 4000 lines plus the continuation hint — this would be a full
     * dump with no hint on pre-T1 code. */
    char ws[] = "/tmp/echo_rf_trunc_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char *abs_path = make_sample_file(ws, "big.txt", SAMPLE_20);
    char abs_arg[600];
    snprintf(abs_arg, sizeof(abs_arg),
             "{\"file_path\":\"big.txt\",\"limit\":5}");
    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_read_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, abs_arg);
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content,
        "l1\nl2\nl3\nl4\nl5\n\n"
        "[Showing lines 1-5 of 21. Use offset=6 to continue.]");
    tool_result_free(r);

    free(abs_path);
    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_read_file_offset_past_eof_is_validation_error)
{
    /* T1: offset beyond the last line is an explicit error naming the
     * line count, not a silently empty window. */
    char ws[] = "/tmp/echo_rf_eof_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char *abs_path = make_sample_file(ws, "w.txt", SAMPLE_20);

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_read_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(
        tool, "{\"file_path\":\"w.txt\",\"offset\":999}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    ck_assert_ptr_nonnull(strstr(r->error, "21 lines total"));
    tool_result_free(r);

    free(abs_path);
    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_read_file_invalid_window_args_rejected)
{
    /* T1: fractional and non-positive offset/limit must be validation
     * errors, not silently truncated to something surprising. */
    char ws[] = "/tmp/echo_rf_bad_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char *abs_path = make_sample_file(ws, "w.txt", SAMPLE_20);

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_read_file_create(safety);
    ck_assert_ptr_nonnull(tool);

    ToolResult *r = tool->execute(tool,
        "{\"file_path\":\"w.txt\",\"offset\":0.5}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);

    r = tool->execute(tool, "{\"file_path\":\"w.txt\",\"limit\":0}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);

    free(abs_path);
    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_read_file_short_read_reports_error)
{
    /* T1e regression: a short fread (file changed under us) must be an
     * error, not a silently truncated dump. Pre-fix code returns the
     * partial content as success. */
    char ws[] = "/tmp/echo_rf_short_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char *abs_path = make_sample_file(ws, "w.txt", "0123456789");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_read_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    read_file_test_set_fread_fail(1);
    ToolResult *r = tool->execute(tool, "{\"file_path\":\"w.txt\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "execution_error");
    tool_result_free(r);
    read_file_test_set_fread_fail(0);

    free(abs_path);
    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
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
    tcase_add_test(tc, test_read_file_offset_limit_pagination);
    tcase_add_test(tc, test_read_file_truncation_notice_without_limit);
    tcase_add_test(tc, test_read_file_offset_past_eof_is_validation_error);
    tcase_add_test(tc, test_read_file_invalid_window_args_rejected);
    tcase_add_test(tc, test_read_file_short_read_reports_error);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
