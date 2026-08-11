/*
 * session_purge.c - session deletion and age-based purge.
 * Depends on: sqlite3, time, session_manager types.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

#include "session_purge.h"
#include "session_manager_internal.h"
#include "session_manager.h"
#include "../utils/logging.h"

#ifdef SESSION_MANAGER_TEST
/* Shared fault-injection hook (counters live in session_manager.c). */
#define sqlite3_bind_text sm_test_bind_text
#endif


int session_manager_delete_session(SessionManager *sm, const char *id)
{
    if (!sm || !id || !sm->db) return -1;

    sm_lock(sm);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "DELETE FROM agent_sessions WHERE id = ?";
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("sqlite prepare delete", "err", sqlite3_errmsg(sm->db), NULL);
        sm_unlock(sm);
        return -1;
    }

    int bind_rc = sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    if (bind_rc != SQLITE_OK)
    {
        log_error("sqlite bind delete", "err", sqlite3_errmsg(sm->db), NULL);
        sqlite3_finalize(stmt);
        sm_unlock(sm);
        return -1;
    }

    int rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(sm->db);
    sqlite3_finalize(stmt);
    sm_unlock(sm);

    if (rc != SQLITE_DONE)
    {
        log_error("sqlite step delete", "err", sqlite3_errmsg(sm->db), NULL);
        return -1;
    }
    return (changed > 0) ? 1 : 0;
}

int session_manager_purge_sessions(SessionManager *sm, int older_than_days)
{
    if (!sm || !sm->db) return -1;

    /* B9: validate the input so the (time_t)older_than_days * 86400
     * multiplication can't overflow for huge or negative values. The purge
     * semantics only make sense for older_than_days >= 0; anything else is
     * operator error and refuses rather than risking UB or a flipped cutoff. */
    if (older_than_days < 0 || older_than_days > 365 * 100)
    {
        log_error("purge_sessions refusing bad older_than_days",
                  "days", "out of range [0, 36500]", NULL);
        return -1;
    }

    time_t cutoff = time(NULL) - (time_t)older_than_days * 86400;
    /* D3: localtime_r is the thread-safe variant — localtime under a
     * multi-threaded server races on the shared static buffer. */
    struct tm tm_storage;
    struct tm *tm_cutoff = localtime_r(&cutoff, &tm_storage);
    if (!tm_cutoff) return -1;

    char cutoff_str[64];
    strftime(cutoff_str, sizeof(cutoff_str), "%Y-%m-%dT%H:%M:%S", tm_cutoff);

    sm_lock(sm);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "DELETE FROM agent_sessions WHERE created_at < ?";
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        /* H2: SQLite guarantees *ppStmt == NULL on prepare error so the
         * `sqlite3_finalize(stmt)` here is a no-op defensively kept for
         * review consistency (every other error path in this function
         * finalizes the stmt before returning). Also log the prepare
         * failure — the prior code returned -1 with no operator signal. */
        log_error("sqlite prepare purge", "err", sqlite3_errmsg(sm->db), NULL);
        sqlite3_finalize(stmt);
        sm_unlock(sm);
        return -1;
    }

    int bind_rc = sqlite3_bind_text(stmt, 1, cutoff_str, -1, SQLITE_TRANSIENT);
    if (bind_rc != SQLITE_OK)
    {
        log_error("sqlite bind purge", "err", sqlite3_errmsg(sm->db), NULL);
        sqlite3_finalize(stmt);
        sm_unlock(sm);
        return -1;
    }

    int step_rc = sqlite3_step(stmt);
    int deleted = 0;
    if (step_rc == SQLITE_DONE)
        deleted = sqlite3_changes(sm->db);
    else
        log_error("sqlite step purge", "err", sqlite3_errmsg(sm->db), NULL);
    sqlite3_finalize(stmt);
    sm_unlock(sm);

    return (step_rc == SQLITE_DONE) ? deleted : -1;
}
