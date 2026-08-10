/*
 * test_tool_sqlite_schema.c - unit tests for the sqlite_schema tool:
 * lists tables with their CREATE SQL and column info from an in-memory
 * DB. Depends on: check, tool.h, registry, sqlite3.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include "tools/tool.h"
#include "tools/registry.h"
#include "session/session_manager.h"

Tool *tool_sqlite_schema_create(SafetyConfig *safety);

static void register_memory_db(void)
{
    static SessionManager sm;
    static int initialized = 0;
    if (initialized) return;
    memset(&sm, 0, sizeof(sm));
    ck_assert_int_eq(sqlite3_open(":memory:", &sm.db), SQLITE_OK);
    ck_assert_int_eq(sqlite3_exec(sm.db,
        "CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL);",
        NULL, NULL, NULL), SQLITE_OK);
    registry_set_session_manager(&sm);
    initialized = 1;
}

START_TEST(test_sqlite_schema_lists_tables_and_columns)
{
    register_memory_db();
    Tool *tool = tool_sqlite_schema_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    cJSON *out = cJSON_Parse(r->content);
    ck_assert_ptr_nonnull(out);
    cJSON *tables = cJSON_GetObjectItem(out, "tables");
    ck_assert_ptr_nonnull(tables);
    ck_assert(cJSON_IsArray(tables));
    ck_assert_int_eq(cJSON_GetArraySize(tables), 1);
    cJSON *table = cJSON_GetArrayItem(tables, 0);
    ck_assert_ptr_nonnull(table);
    cJSON *name = cJSON_GetObjectItem(table, "name");
    ck_assert_ptr_nonnull(name);
    ck_assert_str_eq(name->valuestring, "people");
    cJSON *sql = cJSON_GetObjectItem(table, "sql");
    ck_assert_ptr_nonnull(sql);
    ck_assert(strstr(sql->valuestring, "CREATE TABLE people") != NULL);
    cJSON *cols = cJSON_GetObjectItem(table, "columns");
    ck_assert_ptr_nonnull(cols);
    ck_assert_int_eq(cJSON_GetArraySize(cols), 2);
    cJSON *id_col = cJSON_GetArrayItem(cols, 0);
    cJSON *pk = cJSON_GetObjectItem(id_col, "pk");
    ck_assert_ptr_nonnull(pk);
    ck_assert_int_eq(pk->valueint, 1);
    cJSON_Delete(out);
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_sqlite_schema_excludes_sqlite_internal_tables)
{
    register_memory_db();
    Tool *tool = tool_sqlite_schema_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    cJSON *out = cJSON_Parse(r->content);
    ck_assert_ptr_nonnull(out);
    cJSON *tables = cJSON_GetObjectItem(out, "tables");
    ck_assert_ptr_nonnull(tables);
    for (int i = 0; i < cJSON_GetArraySize(tables); i++)
    {
        cJSON *t = cJSON_GetArrayItem(tables, i);
        cJSON *n = cJSON_GetObjectItem(t, "name");
        ck_assert(n && n->valuestring);
        ck_assert(strncmp(n->valuestring, "sqlite_", 7) != 0);
    }
    cJSON_Delete(out);
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("SqliteSchema");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_sqlite_schema_lists_tables_and_columns);
    tcase_add_test(tc, test_sqlite_schema_excludes_sqlite_internal_tables);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
