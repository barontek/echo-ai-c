/*
 * rate_limiter.h - per-IP request throttling (fixed window) and
 * unlock-attempt throttling, both persisted in SQLite.
 * Depends on: sqlite3, logging.h.
 */

#ifndef ECHO_RATE_LIMITER_H
#define ECHO_RATE_LIMITER_H

typedef struct RateLimiter RateLimiter;

/**
 * rate_limiter_create - open the rate-limit database and tables
 * @max_requests: per-IP request cap per window; stored as-is, so the
 *   caller must pass a positive value — a non-positive cap makes
 *   rate_limiter_allow reject every request once the IP has a row.
 * @window_secs: fixed-window length in seconds; stored as-is.
 * @db_path: SQLite file path, or NULL for an in-memory database
 *   (":memory:", contents lost on destroy).
 *
 * Creates the rate_buckets and unlock_failures tables if absent. A
 * failure to create unlock_failures is logged but not fatal; the
 * limiter still works for request throttling.
 *
 * Return: caller-owned RateLimiter, or NULL on allocation failure,
 * database open failure, or failure to create rate_buckets. Release
 * with rate_limiter_destroy().
 */
RateLimiter *rate_limiter_create(int max_requests, int window_secs, const char *db_path);

/**
 * rate_limiter_destroy - close the database and free the limiter
 * @rl: limiter to free; NULL is a safe no-op.
 *
 * Return: void; never fails.
 */
void rate_limiter_destroy(RateLimiter *rl);

/**
 * rate_limiter_allow - check and consume one request against a per-IP window
 * @rl: limiter to use; NULL reports allowed.
 * @ip: client identifier (e.g. socket peer address); NULL reports
 *   allowed.
 *
 * Applies the fixed window: an IP whose window has expired gets a fresh
 * window with its count reset to 1; otherwise the count is incremented
 * and the request is allowed unless the count already reached
 * max_requests.
 *
 * Return: 1 to allow the request, 0 to reject (limit reached). Fails
 * open: NULL arguments and SQLite errors return 1 (logged on error).
 * Thread-safety: all state lives in one shared sqlite3 connection with
 * no internal lock — the caller must serialize access to a given
 * RateLimiter.
 */
int rate_limiter_allow(RateLimiter *rl, const char *ip);

/**
 * rate_limiter_record_unlock_failure - record a failed unlock attempt
 * @rl: limiter to use; NULL is a no-op.
 * @ip: client identifier; the row is recorded under this key.
 *
 * Timestamps the failure; rate_limiter_unlock_allowed() counts these
 * rows within a rolling 60-second window.
 *
 * Return: void; never fails — SQLite errors are silently swallowed.
 * Thread-safety: same shared-connection caveat as rate_limiter_allow.
 */
void rate_limiter_record_unlock_failure(RateLimiter *rl, const char *ip);

/**
 * rate_limiter_unlock_allowed - check unlock attempts against limits
 * @rl: limiter to use; NULL reports allowed.
 * @ip: client identifier to check against the per-IP limit.
 * @max_per_ip: max unlock failures for this IP within the last 60
 *   seconds.
 * @max_global: max unlock failures across all IPs within the last 60
 *   seconds.
 *
 * The 60-second window is fixed, not a parameter. Both limits must be
 * under to allow; the check does not record anything.
 *
 * Return: 1 to allow the unlock attempt, 0 to reject. Fails open: NULL
 * arguments and SQLite errors return 1 (no log on the SQLite path).
 * Thread-safety: same shared-connection caveat as rate_limiter_allow.
 */
int rate_limiter_unlock_allowed(RateLimiter *rl, const char *ip,
                                 int max_per_ip, int max_global);

#endif
