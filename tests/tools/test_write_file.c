/*
 * test_write_file.c - unit tests for the write_file tool: content
 * writes, content size caps, missing args, and change-tracker snapshot
 * when attached. Depends on: check, tool.h, safety, config,
 * change_tracker.
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
#include "change_tracker/change_tracker.h"

Tool *tool_write_file_create(SafetyConfig *safety);
void tool_write_file_set_change_tracker(Tool *tool, ChangeTracker *ct);

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

START_TEST(test_write_file_creates_file_with_content)
{
    char ws[] = "/tmp/echo_wf_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_write_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"file_path\":\"out.txt\",\"content\":\"data 123\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    tool_result_free(r);

    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/out.txt", ws),
                     (int)sizeof(abs_path));
    char *written = read_whole_file(abs_path);
    ck_assert_str_eq(written, "data 123");
    free(written);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_write_file_creates_file_with_content_exact)
{
    char ws[] = "/tmp/echo_wf_exact_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_write_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"file_path\":\"out.txt\",\"content\":\"data 123\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content, "Written 8 bytes to out.txt");
    tool_result_free(r);

    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/out.txt", ws),
                     (int)sizeof(abs_path));
    char *written = read_whole_file(abs_path);
    ck_assert_str_eq(written, "data 123");
    free(written);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_write_file_overwrites_existing_content)
{
    char ws[] = "/tmp/echo_wf_over_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/out.txt", ws),
                     (int)sizeof(abs_path));
    FILE *f = fopen(abs_path, "w");
    ck_assert_ptr_nonnull(f);
    fputs("old content", f);
    fclose(f);

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_write_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"file_path\":\"out.txt\",\"content\":\"new\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    tool_result_free(r);

    char *written = read_whole_file(abs_path);
    ck_assert_str_eq(written, "new");
    free(written);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_write_file_oversized_content_is_policy_denied)
{
    char ws[] = "/tmp/echo_wf_big_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    SafetyConfig *safety = make_safety(ws);
    safety->max_file_size = 10;
    Tool *tool = tool_write_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"file_path\":\"x.txt\",\"content\":\"01234567890123456789\"}");
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

START_TEST(test_write_file_snapshots_previous_content_to_change_tracker)
{
    char ws[] = "/tmp/echo_wf_ct_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/out.txt", ws),
                     (int)sizeof(abs_path));
    FILE *f = fopen(abs_path, "w");
    ck_assert_ptr_nonnull(f);
    fputs("before", f);
    fclose(f);

    ChangeTracker ct;
    memset(&ct, 0, sizeof(ct));

    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_write_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    tool_write_file_set_change_tracker(tool, &ct);
    ToolResult *r = tool->execute(tool,
        "{\"file_path\":\"out.txt\",\"content\":\"after\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    tool_result_free(r);

    ck_assert_int_eq(ct.undo_count, 1);
    ck_assert_str_eq(ct.undo_stack[0].previous_content, "before");
    /* ct_snapshot mallocs the entry fields; the stack tracker never
     * frees them (Linux LSan caught this — invisible on macOS). */
    free(ct.undo_stack[0].file_path);
    free(ct.undo_stack[0].previous_content);

    tool->destroy(tool);
    safety_config_free(safety);
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_write_file_missing_args_is_validation_error)
{
    char ws[] = "/tmp/echo_wf_val_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    SafetyConfig *safety = make_safety(ws);
    Tool *tool = tool_write_file_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"file_path\":\"/tmp/x\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);

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
    Suite *suite = suite_create("WriteFile");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_write_file_creates_file_with_content);
    tcase_add_test(tc, test_write_file_creates_file_with_content_exact);
    tcase_add_test(tc, test_write_file_overwrites_existing_content);
    tcase_add_test(tc, test_write_file_oversized_content_is_policy_denied);
    tcase_add_test(tc, test_write_file_snapshots_previous_content_to_change_tracker);
    tcase_add_test(tc, test_write_file_missing_args_is_validation_error);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
