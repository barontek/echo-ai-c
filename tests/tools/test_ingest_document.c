/*
 * test_ingest_document.c - unit tests for the ingest_document tool:
 * content and file ingestion into the semantic index, plus validation
 * errors. Depends on: check, tool.h, safety, semantic_search.
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
#include "tools/semantic_search.h"

Tool *tool_ingest_document_create(SafetyConfig *safety);

START_TEST(test_ingest_document_content_succeeds)
{
    SafetyConfig *safety = safety_config_create();
    ck_assert_ptr_nonnull(safety);
    Tool *tool = tool_ingest_document_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(
        tool, "{\"content\":\"some document text\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert(strstr(r->content, "ingested") != NULL);
    tool_result_free(r);
    tool->destroy(tool);
    safety_config_free(safety);
    semantic_search_test_reset();
}
END_TEST

START_TEST(test_ingest_document_file_path_succeeds)
{
    char ws[] = "/tmp/echo_ingest_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(ws));
    char abs_path[512];
    ck_assert_int_lt(snprintf(abs_path, sizeof(abs_path), "%s/doc.txt", ws),
                     (int)sizeof(abs_path));
    FILE *f = fopen(abs_path, "w");
    ck_assert_ptr_nonnull(f);
    fputs("file based document", f);
    fclose(f);

    SafetyConfig *safety = safety_config_create();
    ck_assert_ptr_nonnull(safety);
    safety->workspace = str_dup(ws);
    Tool *tool = tool_ingest_document_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"path\":\"doc.txt\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    tool_result_free(r);
    tool->destroy(tool);
    safety_config_free(safety);
    semantic_search_test_reset();
    char cmd[600];
    ck_assert_int_lt(snprintf(cmd, sizeof(cmd), "rm -rf %s", ws),
                     (int)sizeof(cmd));
    ck_assert_int_eq(system(cmd), 0);
}
END_TEST

START_TEST(test_ingest_document_missing_args_is_validation_error)
{
    SafetyConfig *safety = safety_config_create();
    ck_assert_ptr_nonnull(safety);
    Tool *tool = tool_ingest_document_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);
    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

START_TEST(test_ingest_document_empty_content_is_validation_error)
{
    SafetyConfig *safety = safety_config_create();
    ck_assert_ptr_nonnull(safety);
    Tool *tool = tool_ingest_document_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"content\":\"\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);
    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("IngestDocument");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_ingest_document_content_succeeds);
    tcase_add_test(tc, test_ingest_document_file_path_succeeds);
    tcase_add_test(tc, test_ingest_document_missing_args_is_validation_error);
    tcase_add_test(tc, test_ingest_document_empty_content_is_validation_error);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
