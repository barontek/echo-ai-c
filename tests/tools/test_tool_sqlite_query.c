/*
 * test_tool_sqlite_query.c - unit tests for the sqlite_query tool:
 * read-only SELECT over an in-memory DB, write-statement rejection, and
 * SQL error paths. Depends on: check, tool.h, registry, sqlite3.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include "tools/tool.h"
#include "tools/registry.h"
#include "session/session_manager.h"

Tool *tool_sqlite_query_create(SafetyConfig *safety);

static void register_memory_db(void)
{
    static SessionManager sm;
    static int initialized = 0;
    if (initialized) return;
    memset(&sm, 0, sizeof(sm));
    ck_assert_int_eq(sqlite3_open(":memory:", &sm.db), SQLITE_OK);
    ck_assert_int_eq(sqlite3_exec(sm.db,
        "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT);"
        "INSERT INTO t VALUES (1, 'one');"
        "INSERT INTO t VALUES (2, 'two');", NULL, NULL, NULL), SQLITE_OK);
    registry_set_session_manager(&sm);
    initialized = 1;
}

START_TEST(test_sqlite_query_returns_rows_as_json)
{
    register_memory_db();
    Tool *tool = tool_sqlite_query_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"query\":\"SELECT id, name FROM t ORDER BY id\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    cJSON *out = cJSON_Parse(r->content);
    ck_assert_ptr_nonnull(out);
    cJSON *count = cJSON_GetObjectItem(out, "count");
    ck_assert_ptr_nonnull(count);
    ck_assert_int_eq(count->valueint, 2);
    cJSON *rows = cJSON_GetObjectItem(out, "rows");
    ck_assert_ptr_nonnull(rows);
    cJSON *first = cJSON_GetArrayItem(rows, 0);
    ck_assert_ptr_nonnull(first);
    cJSON *name = cJSON_GetObjectItem(first, "name");
    ck_assert_ptr_nonnull(name);
    ck_assert_str_eq(name->valuestring, "one");
    cJSON_Delete(out);
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_sqlite_query_rejects_non_select)
{
    register_memory_db();
    Tool *tool = tool_sqlite_query_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"query\":\"DELETE FROM t\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "policy_denied");
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_sqlite_query_accepts_pragma_and_explain)
{
    register_memory_db();
    Tool *tool = tool_sqlite_query_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"query\":\"PRAGMA table_info('t')\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    tool_result_free(r);
    r = tool->execute(tool, "{\"query\":\"EXPLAIN SELECT * FROM t\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_sqlite_query_sql_error_is_execution_error)
{
    register_memory_db();
    Tool *tool = tool_sqlite_query_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool,
        "{\"query\":\"SELECT * FROM nonexistent_table\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "execution_error");
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_sqlite_query_missing_query_is_validation_error)
{
    register_memory_db();
    Tool *tool = tool_sqlite_query_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("SqliteQuery");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_sqlite_query_returns_rows_as_json);
    tcase_add_test(tc, test_sqlite_query_rejects_non_select);
    tcase_add_test(tc, test_sqlite_query_accepts_pragma_and_explain);
    tcase_add_test(tc, test_sqlite_query_sql_error_is_execution_error);
    tcase_add_test(tc, test_sqlite_query_missing_query_is_validation_error);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
