#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rate_limiter.h"

#define MAX_IPS 4096

typedef struct {
    char ip[64];
    time_t window_start;
    int count;
} Bucket;

struct RateLimiter {
    Bucket buckets[MAX_IPS];
    int count;
    int max_requests;
    int window_secs;
};

RateLimiter *rate_limiter_create(int max_requests, int window_secs)
{
    RateLimiter *rl = calloc(1, sizeof(RateLimiter));
    if (!rl) return NULL;
    rl->max_requests = max_requests;
    rl->window_secs = window_secs;
    return rl;
}

void rate_limiter_destroy(RateLimiter *rl)
{
    free(rl);
}

int rate_limiter_allow(RateLimiter *rl, const char *ip)
{
    if (!rl || !ip) return 1;

    time_t now = time(NULL);

    for (int i = 0; i < rl->count; i++)
    {
        if (strcmp(rl->buckets[i].ip, ip) == 0)
        {
            if (now - rl->buckets[i].window_start >= rl->window_secs)
            {
                rl->buckets[i].window_start = now;
                rl->buckets[i].count = 1;
                return 1;
            }
            if (rl->buckets[i].count >= rl->max_requests)
                return 0;
            rl->buckets[i].count++;
            return 1;
        }
    }

    if (rl->count < MAX_IPS)
    {
        snprintf(rl->buckets[rl->count].ip, sizeof(rl->buckets[rl->count].ip), "%s", ip);
        rl->buckets[rl->count].window_start = now;
        rl->buckets[rl->count].count = 1;
        rl->count++;
    }

    return 1;
}
