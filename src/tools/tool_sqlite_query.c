#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <cjson/cJSON.h>
#include <sqlite3.h>

#include "tool.h"
#include "registry.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
} SQLiteQueryCtx;

static int is_read_only_query(const char *sql)
{
    const char *p = sql;
    while (*p && isspace((unsigned char)*p)) p++;

    if (strncasecmp(p, "select", 6) == 0) return 1;
    if (strncasecmp(p, "pragma", 6) == 0) return 1;
    if (strncasecmp(p, "explain", 7) == 0) return 1;

    return 0;
}

static ToolResult *sqlite_query_execute(Tool *self, const char *args_json)
{
    (void)self;
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *query = cJSON_GetObjectItem(args, "query");
    if (!query || !cJSON_IsString(query))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'query' argument", "validation_error");
    }

    const char *sql = cJSON_GetStringValue(query);
    if (!is_read_only_query(sql))
    {
        cJSON_Delete(args);
        return tool_result_error("only SELECT queries are allowed", "policy_denied");
    }

    SessionManager *sm = registry_get_session_manager();
    if (!sm)
    {
        cJSON_Delete(args);
        return tool_result_error("session manager not available", "execution_error");
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        cJSON_Delete(args);
        char *err = NULL;
        if (asprintf(&err, "SQL error: %s", sqlite3_errmsg(sm->db)) < 0)
            err = str_dup("SQL error");
        ToolResult *tr = tool_result_error(err, "execution_error");
        free(err);
        return tr;
    }

    cJSON *rows = cJSON_CreateArray();
    if (!rows) { sqlite3_finalize(stmt); cJSON_Delete(args); return tool_result_error("oom", "execution_error"); }

    int col_count = sqlite3_column_count(stmt);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        cJSON *row = cJSON_CreateObject();
        if (!row) break;

        for (int i = 0; i < col_count; i++)
        {
            const char *col_name = sqlite3_column_name(stmt, i);
            int col_type = sqlite3_column_type(stmt, i);

            switch (col_type)
            {
            case SQLITE_INTEGER:
                cJSON_AddNumberToObject(row, col_name, (double)sqlite3_column_int64(stmt, i));
                break;
            case SQLITE_FLOAT:
                cJSON_AddNumberToObject(row, col_name, sqlite3_column_double(stmt, i));
                break;
            case SQLITE_TEXT:
                {
                    const char *txt = (const char *)sqlite3_column_text(stmt, i);
                    cJSON_AddStringToObject(row, col_name, txt ? txt : "");
                }
                break;
            case SQLITE_NULL:
                cJSON_AddNullToObject(row, col_name);
                break;
            default:
                cJSON_AddStringToObject(row, col_name, "");
                break;
            }
        }
        cJSON_AddItemToArray(rows, row);
    }

    sqlite3_finalize(stmt);
    cJSON_Delete(args);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "rows", rows);
    cJSON_AddNumberToObject(result, "count", cJSON_GetArraySize(rows));

    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);

    if (!json) return tool_result_error("oom", "execution_error");
    ToolResult *tr = tool_result_create(json);
    free(json);
    return tr;
}

static void sqlite_query_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

Tool *tool_sqlite_query_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    SQLiteQueryCtx *ctx = calloc(1, sizeof(SQLiteQueryCtx));
    if (!ctx) { free(t); return NULL; }
    ctx->safety = safety;

    t->name = str_dup("sqlite_query");
    t->description = str_dup("Execute a read-only SQL SELECT query on the session database");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"query\":{\"type\":\"string\",\"description\":\"SELECT SQL query to execute\"}"
        "},\"required\":[\"query\"]}"
    );
    t->execute = sqlite_query_execute;
    t->destroy = sqlite_query_destroy;
    t->ctx = ctx;
    return t;
}
