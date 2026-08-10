/*
 * memory.h - persistent user-memory key/value store (user_memory table in
 * the session DB) for user-defined facts injected into the agent's prompt.
 * Depends on: sqlite3.
 */

#ifndef ECHO_MEMORY_H
#define ECHO_MEMORY_H

#include <sqlite3.h>

/* One row of user memory; both strings are caller-owned when returned from
 * memory_list_all() and freed by memory_facts_free(). */
typedef struct {
    char *key;
    char *value;
} MemoryFact;

/**
 * memory_table_init - create the user_memory table if it does not exist
 * @db: open sqlite3 connection; must be non-NULL.
 *
 * CREATE TABLE IF NOT EXISTS, so repeated calls are harmless. Run once
 * before any other memory_* call on the connection.
 *
 * Return: 0 on success, -1 on sqlite3_exec failure (logged). Thread-safe
 * with respect to other memory_* calls on the same connection only if the
 * caller serializes access (session_manager does via sm->lock).
 */
int memory_table_init(sqlite3 *db);

/**
 * memory_set - upsert a key/value pair
 * @db: open sqlite3 connection; must be non-NULL.
 * @key: fact key; must be non-NULL, borrowed for the call duration.
 * @value: fact value; must be non-NULL, borrowed for the call duration.
 *
 * INSERT ... ON CONFLICT(key) DO UPDATE, refreshing updated_at. The value
 * is stored verbatim (no encryption — user memory is not secret).
 *
 * Return: 0 on success, -1 on NULL arguments, prepare failure (logged), or
 * step failure. Thread-safe via caller's lock on the connection.
 */
int memory_set(sqlite3 *db, const char *key, const char *value);

/**
 * memory_get - fetch a value by key
 * @db: open sqlite3 connection; must be non-NULL.
 * @key: fact key; must be non-NULL, borrowed for the call duration.
 *
 * Return: caller-owned str_dup of the stored value (free with free()), or
 * NULL when the key is absent, on NULL arguments, prepare failure, or
 * allocation failure. Thread-safe via caller's lock on the connection.
 */
char *memory_get(sqlite3 *db, const char *key);

/**
 * memory_delete - remove a key/value pair
 * @db: open sqlite3 connection; must be non-NULL.
 * @key: fact key; must be non-NULL, borrowed for the call duration.
 *
 * Return: 0 on success (whether or not a row matched), -1 on NULL
 * arguments, prepare failure, or step failure. Thread-safe via caller's
 * lock on the connection.
 */
int memory_delete(sqlite3 *db, const char *key);

/**
 * memory_list_all - fetch all facts, newest first
 * @db: open sqlite3 connection; must be non-NULL.
 * @count: out-param receiving the number of facts; must be non-NULL.
 *
 * Rows are ordered by updated_at DESC. *count is set to 0 up front; on
 * success it holds the number of facts. On failure the facts gathered so
 * far are freed internally and NULL is returned — *count is unspecified on
 * failure, so callers must not free or index anything.
 *
 * Return: caller-owned array of MemoryFact (free with memory_facts_free()),
 * or NULL on NULL arguments, prepare failure, or any allocation failure.
 * Thread-safe via caller's lock on the connection.
 */
MemoryFact *memory_list_all(sqlite3 *db, int *count);

/**
 * memory_facts_free - free a fact array returned by memory_list_all()
 * @facts: array to free, or NULL (no-op).
 * @count: number of entries in @facts.
 *
 * Frees each key/value string, then the array itself. After the call
 * @facts is dangling.
 *
 * Return: void. Thread-safe; touches only @facts.
 */
void memory_facts_free(MemoryFact *facts, int count);

#ifdef MEMORY_TEST
/**
 * memory_test_set_alloc_fail - arm str_dup fault injection
 * @nth_allocation: fail the Nth str_dup call (1-based), or -1 to disarm.
 *
 * Test-only hook: makes the Nth str_dup inside this module return NULL so
 * allocation-failure paths (e.g. memory_list_all's mid-loop cleanup) can be
 * exercised deterministically. Only compiled with -DMEMORY_TEST=1.
 *
 * Return: void. Not thread-safe; call before any concurrent memory_* use.
 */
void memory_test_set_alloc_fail(int nth_allocation);
#endif

#endif
