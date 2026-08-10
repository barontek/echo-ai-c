/*
 * rate_limiter.c - per-IP request throttling (fixed window) and
 * unlock-attempt throttling (rolling 60 s window), persisted in SQLite;
 * all error paths fail open (allow). Depends on: sqlite3, logging.h.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

#include "rate_limiter.h"
#include "../utils/logging.h"

struct RateLimiter {
    sqlite3 *db;
    int max_requests;
    int window_secs;
};

RateLimiter *rate_limiter_create(int max_requests, int window_secs, const char *db_path)
{
    RateLimiter *rl = calloc(1, sizeof(RateLimiter));
    if (!rl) return NULL;

    rl->max_requests = max_requests;
    rl->window_secs = window_secs;

    const char *path = db_path ? db_path : ":memory:";
    if (sqlite3_open(path, &rl->db) != SQLITE_OK)
    {
        log_error("rate_limiter: failed to open db", "path", path, NULL);
        free(rl);
        return NULL;
    }

    const char *sql_rate = "CREATE TABLE IF NOT EXISTS rate_buckets ("
                           "ip TEXT PRIMARY KEY,"
                           "window_start INTEGER,"
                           "count INTEGER"
                           ")";
    char *err = NULL;
    if (sqlite3_exec(rl->db, sql_rate, NULL, NULL, &err) != SQLITE_OK)
    {
        log_error("rate_limiter: create rate_buckets", "err", err, NULL);
        sqlite3_free(err);
        sqlite3_close(rl->db);
        free(rl);
        return NULL;
    }

    const char *sql_unlock = "CREATE TABLE IF NOT EXISTS unlock_failures ("
                             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                             "ip TEXT,"
                             "timestamp INTEGER"
                             ")";
    if (sqlite3_exec(rl->db, sql_unlock, NULL, NULL, &err) != SQLITE_OK)
    {
        log_error("rate_limiter: create unlock_failures", "err", err, NULL);
        sqlite3_free(err);
    }

    return rl;
}

void rate_limiter_destroy(RateLimiter *rl)
{
    if (!rl) return;
    if (rl->db) sqlite3_close(rl->db);
    free(rl);
}

int rate_limiter_allow(RateLimiter *rl, const char *ip)
{
    if (!rl || !ip || !rl->db) return 1;

    time_t now = time(NULL);
    sqlite3_stmt *stmt = NULL;

    const char *select_sql = "SELECT window_start, count FROM rate_buckets WHERE ip = ?";
    if (sqlite3_prepare_v2(rl->db, select_sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        /* Fail open: a broken/throttled store must not take the whole
         * service down with it — the request is allowed, loudly logged. */
        log_error("rate_limiter: select prep", "err", sqlite3_errmsg(rl->db), NULL);
        return 1;
    }

    sqlite3_bind_text(stmt, 1, ip, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW)
    {
        time_t window_start = (time_t)sqlite3_column_int64(stmt, 0);
        int count = sqlite3_column_int(stmt, 1);
        sqlite3_finalize(stmt);

        if (now - window_start >= rl->window_secs)
        {
            const char *reset_sql = "UPDATE rate_buckets SET window_start = ?, count = 1 WHERE ip = ?";
            if (sqlite3_prepare_v2(rl->db, reset_sql, -1, &stmt, NULL) != SQLITE_OK) return 1;
            sqlite3_bind_int64(stmt, 1, (sqlite3_int64)now);
            sqlite3_bind_text(stmt, 2, ip, -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            return 1;
        }

        if (count >= rl->max_requests) return 0;

        const char *inc_sql = "UPDATE rate_buckets SET count = count + 1 WHERE ip = ?";
        if (sqlite3_prepare_v2(rl->db, inc_sql, -1, &stmt, NULL) != SQLITE_OK) return 1;
        sqlite3_bind_text(stmt, 1, ip, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return 1;
    }

    sqlite3_finalize(stmt);

    /* OR IGNORE absorbs the race where two requests for a fresh IP both
     * try to insert the same primary key; the row that wins owns the
     * window and the loser's increment is dropped for this request. */
    const char *insert_sql = "INSERT OR IGNORE INTO rate_buckets (ip, window_start, count) VALUES (?, ?, 1)";
    if (sqlite3_prepare_v2(rl->db, insert_sql, -1, &stmt, NULL) != SQLITE_OK) return 1;
    sqlite3_bind_text(stmt, 1, ip, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)now);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return 1;
}

void rate_limiter_record_unlock_failure(RateLimiter *rl, const char *ip)
{
    if (!rl || !rl->db) return;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO unlock_failures (ip, timestamp) VALUES (?, ?)";
    if (sqlite3_prepare_v2(rl->db, sql, -1, &stmt, NULL) != SQLITE_OK) return;

    sqlite3_bind_text(stmt, 1, ip, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int rate_limiter_unlock_allowed(RateLimiter *rl, const char *ip,
                                 int max_per_ip, int max_global)
{
    if (!rl || !ip || !rl->db) return 1;

    time_t cutoff = time(NULL) - 60;
    sqlite3_stmt *stmt = NULL;

    const char *per_ip_sql = "SELECT COUNT(*) FROM unlock_failures "
                             "WHERE ip = ? AND timestamp > ?";
    if (sqlite3_prepare_v2(rl->db, per_ip_sql, -1, &stmt, NULL) != SQLITE_OK) return 1;
    sqlite3_bind_text(stmt, 1, ip, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)cutoff);
    int per_ip = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        per_ip = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (per_ip >= max_per_ip) return 0;

    const char *global_sql = "SELECT COUNT(*) FROM unlock_failures WHERE timestamp > ?";
    if (sqlite3_prepare_v2(rl->db, global_sql, -1, &stmt, NULL) != SQLITE_OK) return 1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)cutoff);
    int global = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        global = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (global >= max_global) return 0;

    return 1;
}
