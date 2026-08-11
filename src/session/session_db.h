/*
 * session_db.h - sqlite schema and filesystem plumbing for the session
 * store.
 * Depends on: sqlite3.
 */

#ifndef ECHO_SESSION_DB_H
#define ECHO_SESSION_DB_H

#include <sqlite3.h>

/**
 * mkdir_p - create a directory and all missing parents
 * @path: filesystem path to create.
 *
 * Return: 0 on success (directory exists afterwards), -1 on path
 * overflow or mkdir failure. EEXIST from the final mkdir is not
 * special-cased here; callers tolerate it.
 */
int mkdir_p(const char *path);

/**
 * init_db - create the session store schema and durability pragmas
 * @db: open sqlite3 handle.
 *
 * Creates agent_sessions and provider_oauth if absent and applies
 * journal_mode=DELETE / synchronous=FULL. Failures are logged.
 *
 * Return: 0 on success, -1 if a CREATE TABLE failed (pragma failures
 * are logged but tolerated).
 */
int init_db(sqlite3 *db);

#endif /* ECHO_SESSION_DB_H */
