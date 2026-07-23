#ifndef ECHO_RATE_LIMITER_H
#define ECHO_RATE_LIMITER_H

typedef struct RateLimiter RateLimiter;

RateLimiter *rate_limiter_create(int max_requests, int window_secs);
void rate_limiter_destroy(RateLimiter *rl);
int rate_limiter_allow(RateLimiter *rl, const char *ip);

#endif
