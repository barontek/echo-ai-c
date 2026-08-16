/*
 * fuzz_cdp_line.c - libFuzzer target for the CDP per-line dispatcher
 * (cdp_test_handle_line under CDP_TEST): the pipe transport frames
 * external input from a network-ish source, so the dispatch path gets
 * the same fuzz treatment as the websocket frame parser. The client
 * struct is stack-built without a reader thread; only the dispatch
 * function is exercised.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "browser/cdp.h"

#define MAX_INPUT 65536

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    if (size > MAX_INPUT) return 0;

    CdpClient c;
    memset(&c, 0, sizeof(c));
    if (pthread_mutex_init(&c.mu, NULL) != 0) return 0;
    if (pthread_cond_init(&c.cv, NULL) != 0)
    {
        pthread_mutex_destroy(&c.mu);
        return 0;
    }

    char *buf = malloc(size + 1);
    if (!buf)
    {
        pthread_cond_destroy(&c.cv);
        pthread_mutex_destroy(&c.mu);
        return 0;
    }
    memcpy(buf, data, size);
    buf[size] = '\0';

    cdp_test_handle_line(&c, buf);

    for (size_t i = 0; i < c.pending_count; i++)
        free(c.pending[i].json);

    free(buf);
    pthread_cond_destroy(&c.cv);
    pthread_mutex_destroy(&c.mu);
    return 0;
}
