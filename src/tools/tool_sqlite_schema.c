/*
 * tool_sqlite_schema.c - database schema tool: lists all tables of the
 * session database with their CREATE SQL and per-column info, as JSON.
 * Depends on: tool.h, registry, sqlite3, cJSON, string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <sqlite3.h>

#include "tool.h"
#include "registry.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
} SQLiteSchemaCtx;

static ToolResult *sqlite_schema_execute(Tool *self, const char *args_json)
{
    (void)self;
    (void)args_json;

    SessionManager *sm = registry_get_session_manager();
    if (!sm)
        return tool_result_error("session manager not available", "execution_error");

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT name, sql FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name";
    int rc = sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return tool_result_error("failed to query schema", "execution_error");

    cJSON *tables = cJSON_CreateArray();
    if (!tables) { sqlite3_finalize(stmt); return tool_result_error("oom", "execution_error"); }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *tbl_name = (const char *)sqlite3_column_text(stmt, 0);
        const char *tbl_sql = (const char *)sqlite3_column_text(stmt, 1);

        cJSON *t = cJSON_CreateObject();
        if (!t) break;
        cJSON_AddStringToObject(t, "name", tbl_name ? tbl_name : "");
        cJSON_AddStringToObject(t, "sql", tbl_sql ? tbl_sql : "");

        sqlite3_stmt *col_stmt = NULL;
        char *col_sql = NULL;
        if (asprintf(&col_sql, "PRAGMA table_info('%s')", tbl_name ? tbl_name : "") >= 0)
        {
            if (sqlite3_prepare_v2(sm->db, col_sql, -1, &col_stmt, NULL) == SQLITE_OK)
            {
                cJSON *cols = cJSON_CreateArray();
                while (sqlite3_step(col_stmt) == SQLITE_ROW)
                {
                    cJSON *col = cJSON_CreateObject();
                    cJSON_AddStringToObject(col, "name",
                        (const char *)sqlite3_column_text(col_stmt, 1));
                    cJSON_AddStringToObject(col, "type",
                        (const char *)sqlite3_column_text(col_stmt, 2));
                    cJSON_AddBoolToObject(col, "notnull",
                        sqlite3_column_int(col_stmt, 3) != 0);
                    cJSON_AddBoolToObject(col, "pk",
                        sqlite3_column_int(col_stmt, 5) != 0);
                    cJSON_AddItemToArray(cols, col);
                }
                cJSON_AddItemToObject(t, "columns", cols);
                sqlite3_finalize(col_stmt);
            }
            free(col_sql);
        }

        cJSON_AddItemToArray(tables, t);
    }
    sqlite3_finalize(stmt);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tables", tables);
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);

    if (!json) return tool_result_error("oom", "execution_error");
    ToolResult *tr = tool_result_create(json);
    free(json);
    return tr;
}

static void sqlite_schema_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_sqlite_schema_create - construct the sqlite_schema tool
 * @safety: borrowed SafetyConfig; retained in the tool's context but not
 * consulted by the execute path
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
Tool *tool_sqlite_schema_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    SQLiteSchemaCtx *ctx = calloc(1, sizeof(SQLiteSchemaCtx));
    if (!ctx) { free(t); return NULL; }
    ctx->safety = safety;

    t->name = str_dup("sqlite_schema");
    t->description = str_dup("Get the database schema showing all tables, columns, and their types");
    t->parameters_schema = str_dup("{\"type\":\"object\",\"properties\":{}}");
    t->execute = sqlite_schema_execute;
    t->destroy = sqlite_schema_destroy;
    t->ctx = ctx;
    return t;
}
