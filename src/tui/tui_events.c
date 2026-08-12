/*
 * tui_events.c - fixed-capacity event ring between the worker thread and
 * the UI thread. Push is blocking (chunks stream faster than the UI
 * renders; dropping is unacceptable), pop is nonblocking, and a self-pipe
 * byte is written per successful enqueue so the UI loop can poll without
 * holding the ring lock.
 *
 * Round-trip events carry their own mutex/condvar: the worker waits on the
 * event (not the ring) after pushing, and the UI answers it after popping.
 * Depends on: pthreads, unistd.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "tui_events.h"
#include "../utils/string_utils.h"

#ifdef TUI_EVENTS_TEST
/* Fault-injection shims: the test TU provides these so allocation
 * failures can be forced at each copy site of tui_events_push. */
void *tui_events_test_calloc(size_t nmemb, size_t size);
char *tui_events_test_strdup(const char *s);
#define calloc tui_events_test_calloc
#define str_dup tui_events_test_strdup
#endif

struct TuiEvents {
    TuiEvent **ring;
    size_t capacity;
    size_t head; /* next slot to pop */
    size_t count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int wake_fds[2];
};

static void ev_strings_free(TuiEvent *ev)
{
    free(ev->text);
    free(ev->extra);
    free(ev->answer);
    ev->text = NULL;
    ev->extra = NULL;
    ev->answer = NULL;
}

/* Copy both payload strings; on failure free the partial copy. */
static int ev_fill(TuiEvent *ev, const char *text, const char *extra)
{
    ev->text = str_dup(text ? text : "");
    if (!ev->text) return -1;
    if (extra)
    {
        ev->extra = str_dup(extra);
        if (!ev->extra)
        {
            ev_strings_free(ev);
            return -1;
        }
    }
    return 0;
}
TuiEvents *tui_events_init(size_t capacity)
{
    if (capacity < 1) return NULL;

    TuiEvents *evs = calloc(1, sizeof(TuiEvents));
    if (!evs) return NULL;

    evs->ring = calloc(capacity, sizeof(TuiEvent *));
    if (!evs->ring)
    {
        free(evs);
        return NULL;
    }
    evs->capacity = capacity;
    evs->head = 0;
    evs->count = 0;
    evs->wake_fds[0] = -1;
    evs->wake_fds[1] = -1;

    if (pipe(evs->wake_fds) != 0)
    {
        free(evs->ring);
        free(evs);
        return NULL;
    }
    /* O_CLOEXEC on both ends: exec'd children (tools) must not inherit */
    for (int i = 0; i < 2; i++)
    {
        int flags = fcntl(evs->wake_fds[i], F_GETFD, 0);
        if (flags < 0 || fcntl(evs->wake_fds[i], F_SETFD, flags | FD_CLOEXEC) != 0)
        {
            close(evs->wake_fds[0]);
            close(evs->wake_fds[1]);
            free(evs->ring);
            free(evs);
            return NULL;
        }
    }
    /* Both ends nonblocking: the write end must never stall the worker on a
     * full pipe, and drain_wake's read loop must return on EAGAIN instead
     * of blocking the UI thread on an empty pipe. */
    for (int i = 0; i < 2; i++)
    {
        int flags = fcntl(evs->wake_fds[i], F_GETFL, 0);
        if (flags < 0 || fcntl(evs->wake_fds[i], F_SETFL, flags | O_NONBLOCK) != 0)
        {
            close(evs->wake_fds[0]);
            close(evs->wake_fds[1]);
            free(evs->ring);
            free(evs);
            return NULL;
        }
    }

    if (pthread_mutex_init(&evs->lock, NULL) != 0 ||
        pthread_cond_init(&evs->not_empty, NULL) != 0 ||
        pthread_cond_init(&evs->not_full, NULL) != 0)
    {
        close(evs->wake_fds[0]);
        close(evs->wake_fds[1]);
        free(evs->ring);
        free(evs);
        return NULL;
    }
    return evs;
}

void tui_events_destroy(TuiEvents *evs)
{
    if (!evs) return;
    for (size_t i = 0; i < evs->count; i++)
    {
        size_t idx = (evs->head + i) % evs->capacity;
        tui_event_free(evs->ring[idx]);
    }
    free(evs->ring);
    if (evs->wake_fds[0] >= 0) close(evs->wake_fds[0]);
    if (evs->wake_fds[1] >= 0) close(evs->wake_fds[1]);
    pthread_mutex_destroy(&evs->lock);
    pthread_cond_destroy(&evs->not_empty);
    pthread_cond_destroy(&evs->not_full);
    free(evs);
}

TuiEvent *tui_events_push(TuiEvents *evs, TuiEventType type,
                          const char *text, const char *extra)
{
    TuiEvent *ev = calloc(1, sizeof(TuiEvent));
    if (!ev) return NULL;

    if (ev_fill(ev, text, extra) != 0)
    {
        free(ev);
        return NULL;
    }
    ev->type = type;
    ev->round_trip = (type == TUI_EV_ASK_USER || type == TUI_EV_APPROVAL);
    if (ev->round_trip)
    {
        if (pthread_mutex_init(&ev->lock, NULL) != 0 ||
            pthread_cond_init(&ev->cond, NULL) != 0)
        {
            ev_strings_free(ev);
            free(ev);
            return NULL;
        }
    }

    pthread_mutex_lock(&evs->lock);
    while (evs->count == evs->capacity)
        pthread_cond_wait(&evs->not_full, &evs->lock);
    evs->ring[(evs->head + evs->count) % evs->capacity] = ev;
    evs->count++;
    pthread_cond_signal(&evs->not_empty);
    pthread_mutex_unlock(&evs->lock);

    /* Nonblocking: the pipe is 64KB and the ring is capacity-bounded */
    ssize_t n = write(evs->wake_fds[1], "w", 1);
    (void)n;
    return ev;
}

TuiEvent *tui_events_pop(TuiEvents *evs)
{
    pthread_mutex_lock(&evs->lock);
    if (evs->count == 0)
    {
        pthread_mutex_unlock(&evs->lock);
        return NULL;
    }
    TuiEvent *ev = evs->ring[evs->head];
    evs->head = (evs->head + 1) % evs->capacity;
    evs->count--;
    pthread_cond_signal(&evs->not_full);
    pthread_mutex_unlock(&evs->lock);
    return ev;
}

TuiEvent *tui_events_pop_blocking(TuiEvents *evs)
{
    pthread_mutex_lock(&evs->lock);
    while (evs->count == 0)
        pthread_cond_wait(&evs->not_empty, &evs->lock);
    TuiEvent *ev = evs->ring[evs->head];
    evs->head = (evs->head + 1) % evs->capacity;
    evs->count--;
    pthread_cond_signal(&evs->not_full);
    pthread_mutex_unlock(&evs->lock);
    return ev;
}

void tui_event_free(TuiEvent *ev)
{
    if (!ev) return;
    ev_strings_free(ev);
    if (ev->round_trip)
    {
        pthread_mutex_destroy(&ev->lock);
        pthread_cond_destroy(&ev->cond);
    }
    free(ev);
}

int tui_event_wait_for_answer(TuiEvent *ev, char **answer_out, int *result_out)
{
    if (!ev || !ev->round_trip) return -1;

    pthread_mutex_lock(&ev->lock);
    while (!ev->answered)
        pthread_cond_wait(&ev->cond, &ev->lock);
    if (answer_out)
        *answer_out = ev->answer ? str_dup(ev->answer) : NULL;
    if (result_out) *result_out = ev->result;
    pthread_mutex_unlock(&ev->lock);
    return 0;
}

int tui_event_answer(TuiEvent *ev, const char *answer, int result)
{
    if (!ev || !ev->round_trip) return -1;

    pthread_mutex_lock(&ev->lock);
    free(ev->answer);
    ev->answer = answer ? str_dup(answer) : NULL;
    ev->result = result;
    ev->answered = 1;
    pthread_cond_signal(&ev->cond);
    pthread_mutex_unlock(&ev->lock);
    return 0;
}

int tui_events_wake_fd(const TuiEvents *evs)
{
    return evs->wake_fds[0];
}

void tui_events_drain_wake(TuiEvents *evs)
{
    char buf[64];
    /* Nonblocking read end: loop until EAGAIN (empty pipe) */
    while (read(evs->wake_fds[0], buf, sizeof(buf)) > 0)
        ;
}

int tui_events_empty(const TuiEvents *evs)
{
    return evs->count == 0;
}

