#ifndef ECHO_RATE_LIMITER_H
#define ECHO_RATE_LIMITER_H

typedef struct RateLimiter RateLimiter;

RateLimiter *rate_limiter_create(int max_requests, int window_secs, const char *db_path);
void rate_limiter_destroy(RateLimiter *rl);
int rate_limiter_allow(RateLimiter *rl, const char *ip);

void rate_limiter_record_unlock_failure(RateLimiter *rl, const char *ip);
int rate_limiter_unlock_allowed(RateLimiter *rl, const char *ip,
                                 int max_per_ip, int max_global);

#endif
