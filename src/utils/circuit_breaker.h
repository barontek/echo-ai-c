/*
 * circuit_breaker.h - fail-fast trip state for repeated LLM/provider
 * failures: CLOSED -> OPEN on consecutive failures, OPEN -> HALF_OPEN
 * after a cooldown, HALF_OPEN -> CLOSED on the next success.
 * Depends on: stddef.h, time (monotonic clock).
 */

#ifndef ECHO_CIRCUIT_BREAKER_H
#define ECHO_CIRCUIT_BREAKER_H

#include <stddef.h>

typedef enum {
    CB_CLOSED,
    CB_OPEN,
    CB_HALF_OPEN
} CBState;

typedef struct {
    CBState state;
    int failure_count;
    int failure_threshold;
    int half_open_timeout_ms;
    long long opened_at_ms;
} CircuitBreaker;

/**
 * cb_create - allocate a circuit breaker
 * @failure_threshold: consecutive failures before the breaker trips
 *   OPEN; values <= 0 default to 5.
 * @half_open_timeout_ms: cooldown after tripping before a single probe
 *   request is allowed again; values <= 0 default to 30000.
 *
 * Return: caller-owned CircuitBreaker in CB_CLOSED state with a zero
 * failure count, or NULL on allocation failure. Release with
 * cb_destroy(). No shared state; the returned breaker is standalone.
 */
CircuitBreaker *cb_create(int failure_threshold, int half_open_timeout_ms);

/**
 * cb_destroy - free a circuit breaker
 * @cb: breaker to free; NULL is a safe no-op.
 *
 * Return: void; never fails.
 */
void cb_destroy(CircuitBreaker *cb);

/**
 * cb_is_available - whether a request may proceed
 * @cb: breaker to query; NULL reports available (1).
 *
 * Returns 1 while CLOSED or HALF_OPEN, and 0 while OPEN until
 * half_open_timeout_ms has elapsed since the trip, after which the
 * breaker transitions to HALF_OPEN and returns 1.
 *
 * Return: 1 to allow the request, 0 to fail fast. Never fails.
 * Thread-safety: mutates state on the OPEN -> HALF_OPEN transition, so
 * concurrent calls on the same breaker are not synchronized; the caller
 * must serialize access.
 */
int cb_is_available(CircuitBreaker *cb);

/**
 * cb_record_success - record a successful request
 * @cb: breaker to update; NULL is a safe no-op.
 *
 * Resets the failure count to 0 and returns a HALF_OPEN breaker to
 * CB_CLOSED. A CLOSED breaker stays CLOSED.
 *
 * Return: void; never fails. Thread-safety: mutates state; not
 * synchronized — the caller must serialize access.
 */
void cb_record_success(CircuitBreaker *cb);

/**
 * cb_record_failure - record a failed request
 * @cb: breaker to update; NULL is a safe no-op.
 *
 * Increments the failure count; once it reaches failure_threshold the
 * breaker trips to CB_OPEN and records the trip time (monotonic ms) that
 * drives the half-open cooldown.
 *
 * Return: void; never fails. Thread-safety: mutates state; not
 * synchronized — the caller must serialize access.
 */
void cb_record_failure(CircuitBreaker *cb);

/**
 * cb_now_ms - read the monotonic clock in milliseconds
 *
 * Monotonic source (CLOCK_MONOTONIC), so elapsed-time math is immune to
 * wall-clock jumps.
 *
 * Return: milliseconds since an arbitrary fixed origin; never fails,
 * thread-safe, and shared by all breakers.
 */
long long cb_now_ms(void);

#endif
