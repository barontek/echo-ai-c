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

CircuitBreaker *cb_create(int failure_threshold, int half_open_timeout_ms);
void cb_destroy(CircuitBreaker *cb);
int cb_is_available(CircuitBreaker *cb);
void cb_record_success(CircuitBreaker *cb);
void cb_record_failure(CircuitBreaker *cb);
long long cb_now_ms(void);

#endif
