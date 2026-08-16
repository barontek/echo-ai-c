/*
 * session_db.c - sqlite schema and filesystem plumbing for the
 * session store: data-directory creation and table setup.
 * Depends on: sqlite3, logging.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

#include <sqlite3.h>

#include "session_db.h"
#include "session_manager_internal.h"
#include "../utils/logging.h"


int mkdir_p(const char *path)
{
    char tmp[1024];
    int len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len <= 0 || len >= (int)sizeof(tmp)) return -1;

    /* 0700: the vault directory holds key material (salt, pepper,
     * verifier) and must not be traversable or listable by others. */
    for (int i = 0; i < len; i++)
    {
        if (tmp[i] == '/')
        {
            tmp[i] = '\0';
            mkdir(tmp, 0700);
            tmp[i] = '/';
        }
    }
    return mkdir(path, 0700);
}

int init_db(sqlite3 *db)
{
    const char *sql = "CREATE TABLE IF NOT EXISTS agent_sessions ("
                      "id TEXT PRIMARY KEY,"
                      "title_encrypted BLOB,"
                      "title_generation_attempted INTEGER DEFAULT 0,"
                      "created_at TEXT,"
                      "messages_encrypted BLOB,"
                      "metadata_encrypted BLOB,"
                      "events_encrypted BLOB"
                      ");";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK)
    {
        log_error("sqlite create agent_sessions table", "err", err, NULL);
        sqlite3_free(err);
        return -1;
    }

    const char *oauth_sql = "CREATE TABLE IF NOT EXISTS provider_oauth ("
                            "provider TEXT PRIMARY KEY,"
                            "data_encrypted BLOB NOT NULL"
                            ");";
    if (sqlite3_exec(db, oauth_sql, NULL, NULL, &err) != SQLITE_OK)
    {
        log_error("sqlite create provider_oauth table", "err", err, NULL);
        sqlite3_free(err);
        return -1;
    }

    /* C11: the PRAGMA durability guarantees used to be silently absent
     * if these failed — the session store would still work, just without
     * the crash-durability contract the migration flow relies on. */
    {
        char *prag_err = NULL;
        if (sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, &prag_err) != SQLITE_OK)
        {
            log_error("session_manager: journal_mode PRAGMA failed",
                      "err", prag_err, NULL);
            sqlite3_free(prag_err);
            prag_err = NULL;
        }
        if (sqlite3_exec(db, "PRAGMA synchronous=FULL", NULL, NULL, &prag_err) != SQLITE_OK)
        {
            log_error("session_manager: synchronous PRAGMA failed",
                      "err", prag_err, NULL);
            sqlite3_free(prag_err);
            prag_err = NULL;
        }
    }
    return 0;
}
