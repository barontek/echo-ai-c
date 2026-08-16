/*
 * fake_cdp.h - test-only fake CDP peer: a thread that speaks the pipe
 * protocol (NUL-delimited JSON) over one end of a socketpair and answers
 * commands from a per-test rule table. Lets browser/cdp tests exercise
 * the full protocol stack deterministically with no browser process.
 * Depends on: cJSON, pthreads.
 */

#ifndef ECHO_TEST_FAKE_CDP_H
#define ECHO_TEST_FAKE_CDP_H

#include <pthread.h>
#include <stddef.h>

#include <cjson/cJSON.h>

typedef struct FakeCdpRule {
    const char *match;         /* substring of the method name (first
                                  matching rule wins); NULL matches any */
    cJSON *payload;            /* response {"id":N,"result":payload} */
    const char *error_message; /* non-NULL: respond with an error
                                  instead of a payload */
    const char *prelude;       /* raw JSON message (NUL-framed) written
                                  before the response, e.g. an event */
    int drop;                  /* 1 = never respond (tests timeouts) */
    int chunks;                /* split the response into N writes */
} FakeCdpRule;

typedef struct FakeCdp {
    int fd;                    /* peer fd; the test may write events */
    pthread_t thread;
    FakeCdpRule *rules;        /* borrowed; valid until fake_cdp_stop */
    int rule_count;
    int requests_seen;
} FakeCdp;

/**
 * fake_cdp_start - spawn the fake peer on a socketpair
 * @client_fd_out: receives the client-side fd (hand to cdp_client_new).
 * @rules: rule table; the LAST rule must match any method (NULL match).
 * @count: number of rules.
 *
 * Return: allocated FakeCdp (release with fake_cdp_stop), or NULL.
 */
FakeCdp *fake_cdp_start(int *client_fd_out, FakeCdpRule *rules, int count);

/**
 * fake_cdp_stop - shut the peer down
 * @f: fake, or NULL.
 *
 * Closes the peer fd (the client's reader sees EOF and marks the
 * transport dead), joins the thread and frees. Call before the rule
 * table goes out of scope.
 */
void fake_cdp_stop(FakeCdp *f);

#endif
