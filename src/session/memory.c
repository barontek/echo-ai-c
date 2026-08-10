/*
 * memory.c - persistent user-memory key/value store (user_memory table in
 * the session DB) for user-defined facts injected into the agent's prompt.
 * Depends on: sqlite3, logging, string_utils.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

#ifdef MEMORY_TEST
static int mem_alloc_counter = 0;
static int mem_alloc_fail_at = -1;

void memory_test_set_alloc_fail(int nth_allocation)
{
    mem_alloc_counter = 0;
    mem_alloc_fail_at = nth_allocation;
}

/* Test-only allocator shim: forces the Nth str_dup to return NULL so the
 * allocation-failure paths (memory_list_all's mid-loop cleanup) can be
 * proven; production builds never compile this (see AGENTS.md). */
static char *memory_test_strdup(const char *s)
{
    mem_alloc_counter++;
    if (mem_alloc_counter == mem_alloc_fail_at) return NULL;
    return str_dup(s);
}

#define str_dup memory_test_strdup
#endif

int memory_table_init(sqlite3 *db)
{
    const char *sql = "CREATE TABLE IF NOT EXISTS user_memory ("
                      "key TEXT PRIMARY KEY,"
                      "value TEXT NOT NULL,"
                      "updated_at TEXT DEFAULT CURRENT_TIMESTAMP"
                      ")";
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK)
    {
        log_error("memory table init failed", "error", err ? err : "unknown", NULL);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int memory_set(sqlite3 *db, const char *key, const char *value)
{
    if (!db || !key || !value) return -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO user_memory (key, value, updated_at) "
                      "VALUES (?, ?, CURRENT_TIMESTAMP) "
                      "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=CURRENT_TIMESTAMP";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { log_error("memory_set prepare", NULL); return -1; }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE ? 0 : -1;
}

char *memory_get_dup(sqlite3 *db, const char *key)
{
    if (!db || !key) return NULL;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT value FROM user_memory WHERE key = ?";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    char *result = NULL;
    if (rc == SQLITE_ROW)
    {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val) result = str_dup(val);
    }
    sqlite3_finalize(stmt);
    return result;
}

int memory_delete(sqlite3 *db, const char *key)
{
    if (!db || !key) return -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "DELETE FROM user_memory WHERE key = ?";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

MemoryFact *memory_list_all(sqlite3 *db, int *count)
{
    *count = 0;
    if (!db) return NULL;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT key, value FROM user_memory ORDER BY updated_at DESC";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;

    int cap = 16;
    MemoryFact *facts = malloc(sizeof(MemoryFact) * cap);
    if (!facts) { sqlite3_finalize(stmt); return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        if (*count >= cap)
        {
            cap *= 2;
            MemoryFact *newf = realloc(facts, sizeof(MemoryFact) * cap);
            if (!newf) { memory_facts_free(facts, *count); sqlite3_finalize(stmt); return NULL; }
            facts = newf;
        }
        const char *k = (const char *)sqlite3_column_text(stmt, 0);
        const char *v = (const char *)sqlite3_column_text(stmt, 1);
        char *key_dup = str_dup(k ? k : "");
        char *val_dup = str_dup(v ? v : "");
        if (!key_dup || !val_dup)
        {
            free(key_dup);
            free(val_dup);
            memory_facts_free(facts, *count);
            sqlite3_finalize(stmt);
            return NULL;
        }
        facts[*count].key = key_dup;
        facts[*count].value = val_dup;
        (*count)++;
    }

    sqlite3_finalize(stmt);
    return facts;
}

void memory_facts_free(MemoryFact *facts, int count)
{
    if (!facts) return;
    for (int i = 0; i < count; i++)
    {
        free(facts[i].key);
        free(facts[i].value);
    }
    free(facts);
}
