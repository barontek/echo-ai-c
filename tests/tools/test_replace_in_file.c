/*
 * test_replace_in_file.c - unit tests for the replace_in_file tool:
 * first-occurrence replacement, no-match handling, missing files, and
 * safety rejection. Depends on: check, tool.h, safety, config.
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

Tool *tool_replace_in_file_create(SafetyConfig *safety);
void replace_in_file_test_set_fwrite_fail(int fail);

static SafetyConfig *make_safety(const char *workspace)
{
    SafetyConfig *safety = safety_config_create();
    ck_assert_ptr_nonnull(safety);
    safety->workspace = str_dup(workspace);
    safety->max_file_size = 4096;
    return safety;
}

static char *read_whole_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    ck_assert_ptr_nonnull(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)size + 1);
    ck_assert_ptr_nonnull(buf);
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    ck_assert_ptr_nonnull(f);
    fputs(content, f);
    fclose(f);
}

START_TEST(test_replace_in_file_replaces_first_occurrence)
{
    char ws[] = "/tmp/echo_rif_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path, "foo bar foo");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_replace_in_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"foo\",\"new_string\":\"X\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content, "Replaced 1 occurrence (9 bytes written).");
    tool_result_free(r);

    char *result = read_whole_file(abs_path);
    ck_assert_str_eq(result, "X bar foo");
    free(result);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_replace_in_file_no_match_is_success_notice)
{
    char ws[] = "/tmp/echo_rif_nomatch_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path, "hello");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_replace_in_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"zzz\",\"new_string\":\"X\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content, "No match found for old_string in file.");
    tool_result_free(r);

    char *result = read_whole_file(abs_path);
    ck_assert_str_eq(result, "hello");
    free(result);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_replace_in_file_missing_file_is_file_not_found)
{
    char ws[] = "/tmp/echo_rif_missing_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_replace_in_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"nope.txt\",\"old_string\":\"a\",\"new_string\":\"b\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "file_not_found");
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    ck_assert_int_eq(rmdir(ws), 0);
}
END_TEST

START_TEST(test_replace_in_file_missing_args_is_validation_error)
{
    char ws[] = "/tmp/echo_rif_val_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_replace_in_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"path\":\"/tmp/x\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);

    tool->destroy(tool);
    safety_config_free(safety);
    ck_assert_int_eq(rmdir(ws), 0);
}
END_TEST

/* C2 regression: a short fwrite (disk full) used to be ignored — the tool
 * reported success after silently truncating the file. It must now return
 * an error and leave the file untouched. */
START_TEST(test_replace_in_file_write_failure_reports_error)
{
    char ws[] = "/tmp/echo_rif_write_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/f.txt", ws),
                     (int)sizeof(abs_path));
    write_file(abs_path, "foo bar foo");

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_replace_in_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    replace_in_file_test_set_fwrite_fail(1);
    ToolResult *r = tool->execute(tool,
        "{\"path\":\"f.txt\",\"old_string\":\"foo\",\"new_string\":\"X\"}");
    replace_in_file_test_set_fwrite_fail(0);
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "execution_error");
    tool_result_free(r);

    /* file untouched */
    char *after = read_whole_file(abs_path);
    ck_assert_str_eq(after, "foo bar foo");
    free(after);

    tool->destroy(tool);
    safety_config_free(safety);
    ck_assert_int_eq(unlink(abs_path), 0);
    ck_assert_int_eq(rmdir(ws), 0);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("ReplaceInFile");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_replace_in_file_replaces_first_occurrence);
    tcase_add_test(tc, test_replace_in_file_no_match_is_success_notice);
    tcase_add_test(tc, test_replace_in_file_missing_file_is_file_not_found);
    tcase_add_test(tc, test_replace_in_file_missing_args_is_validation_error);
    tcase_add_test(tc, test_replace_in_file_write_failure_reports_error);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
