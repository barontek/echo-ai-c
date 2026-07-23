#define _GNU_SOURCE
#include <stdlib.h>
#include <time.h>

#include "circuit_breaker.h"

CircuitBreaker *cb_create(int failure_threshold, int half_open_timeout_ms)
{
    CircuitBreaker *cb = calloc(1, sizeof(CircuitBreaker));
    if (!cb) return NULL;
    cb->state = CB_CLOSED;
    cb->failure_threshold = failure_threshold > 0 ? failure_threshold : 5;
    cb->half_open_timeout_ms = half_open_timeout_ms > 0 ? half_open_timeout_ms : 30000;
    return cb;
}

void cb_destroy(CircuitBreaker *cb)
{
    free(cb);
}

int cb_is_available(CircuitBreaker *cb)
{
    if (!cb) return 1;

    switch (cb->state)
    {
    case CB_CLOSED:
        return 1;
    case CB_OPEN:
    {
        long long now = cb_now_ms();
        if (now - cb->opened_at_ms >= cb->half_open_timeout_ms)
        {
            cb->state = CB_HALF_OPEN;
            return 1;
        }
        return 0;
    }
    case CB_HALF_OPEN:
        return 1;
    }
    return 1;
}

void cb_record_success(CircuitBreaker *cb)
{
    if (!cb) return;
    cb->failure_count = 0;
    if (cb->state == CB_HALF_OPEN)
        cb->state = CB_CLOSED;
}

void cb_record_failure(CircuitBreaker *cb)
{
    if (!cb) return;
    cb->failure_count++;
    if (cb->failure_count >= cb->failure_threshold)
    {
        cb->state = CB_OPEN;
        cb->opened_at_ms = cb_now_ms();
    }
}

long long cb_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
