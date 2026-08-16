/*
 * cdp.c - Newline-JSON CDP transport over a pipe pair. Spawned browser
 * speaks on fd 3 (commands in) / fd 4 (messages out); a poll-based
 * reader thread frames lines, stores id-matched responses, and drops
 * events. Depends on: cdp.h, cJSON, string_utils, logging, pthreads.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cdp.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

#ifdef CDP_TEST
static int cdp_strdup_fail_at = -1;
static int cdp_strdup_call_count = 0;

void cdp_test_set_strdup_fail(int nth_allocation)
{
    cdp_strdup_call_count = 0;
    cdp_strdup_fail_at = nth_allocation;
}

static char *cdp_test_strdup(const char *s)
{
    cdp_strdup_call_count++;
    if (cdp_strdup_call_count == cdp_strdup_fail_at) return NULL;
    return str_dup(s);
}

#define str_dup cdp_test_strdup
#endif

#define READ_CHUNK 65536
#define DEFAULT_TIMEOUT_MS 30000
/* Reader wakes at least this often so a close request is noticed even
 * while the browser stays quiet; also bounds shutdown latency. */
#define POLL_INTERVAL_MS 100

/* Mark the transport dead under the lock and wake every waiter. */
static void cdp_set_dead(CdpClient *c, int dead)
{
    pthread_mutex_lock(&c->mu);
    c->dead = dead;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

/* Append bytes to the reader's partial-line accumulator. Returns -1 on
 * OOM, which the reader treats as transport death (a half-framed
 * stream is unrecoverable). */
static int cdp_buf_append(CdpClient *c, const char *data, size_t len)
{
    if (c->line_len + len + 1 > c->line_cap)
    {
        size_t new_cap = c->line_cap ? c->line_cap * 2 : 4096;
        while (new_cap < c->line_len + len + 1) new_cap *= 2;
        char *nb = realloc(c->line_buf, new_cap);
        if (!nb) return -1;
        c->line_buf = nb;
        c->line_cap = new_cap;
    }
    memcpy(c->line_buf + c->line_len, data, len);
    c->line_len += len;
    c->line_buf[c->line_len] = '\0';
    return 0;
}

/* Store a response under the lock, dropping the oldest entry when the
 * bounded queue is full (sequential commands never legitimately fill
 * it; a full queue means a stale response from a timed-out call). */
static void cdp_store_response(CdpClient *c, int id, char *json)
{
    pthread_mutex_lock(&c->mu);
    if (c->pending_count == CDP_MAX_PENDING)
    {
        log_warn("cdp response queue full, dropping oldest", NULL);
        free(c->pending[0].json);
        memmove(&c->pending[0], &c->pending[1],
                (c->pending_count - 1) * sizeof(c->pending[0]));
        c->pending_count--;
    }
    c->pending[c->pending_count].id = id;
    c->pending[c->pending_count].json = json;
    c->pending_count++;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

/* Dispatch one complete CDP line: store responses by id, drop events.
 * Returns 0 for a line that was handled or harmlessly discarded, -1
 * only on OOM (unrecoverable for the stream). */
static int cdp_handle_line(CdpClient *c, const char *line)
{
    cJSON *msg = cJSON_Parse(line);
    if (!msg)
    {
        log_warn("cdp malformed message dropped", "line", line, NULL);
        return 0;
    }

    cJSON *id = cJSON_GetObjectItem(msg, "id");
    if (cJSON_IsNumber(id))
    {
        char *copy = str_dup(line);
        if (!copy)
        {
            cJSON_Delete(msg);
            return -1;
        }
        cdp_store_response(c, (int)id->valuedouble, copy);
    }
    /* Events (method, no id) are intentionally discarded: the browser
     * layer polls state instead of consuming the event stream, which
     * keeps every command synchronous. */
    cJSON_Delete(msg);
    return 0;
}

#ifdef CDP_TEST
int cdp_test_handle_line(CdpClient *c, const char *line)
{
    return cdp_handle_line(c, line);
}
#endif

/* Reader loop: poll() keeps the shutdown check race-free (closing the
 * read fd from another thread while this one blocks in read() could
 * have the fd number reused), frames lines, dispatches them. */
static void *cdp_reader_main(void *arg)
{
    CdpClient *c = arg;
    char chunk[READ_CHUNK];

    for (;;)
    {
        pthread_mutex_lock(&c->mu);
        int shutdown = c->shutdown;
        int dead = c->dead;
        pthread_mutex_unlock(&c->mu);
        if (shutdown || dead) break;

        struct pollfd pfd = {.fd = c->read_fd, .events = POLLIN};
        int pr = poll(&pfd, 1, POLL_INTERVAL_MS);
        if (pr < 0)
        {
            if (errno == EINTR) continue;
            cdp_set_dead(c, 1);
            break;
        }
        if (pr == 0) continue; /* poll interval elapsed, no data */

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            cdp_set_dead(c, 1);
            break;
        }
        if (!(pfd.revents & POLLIN)) continue;

        ssize_t n = read(c->read_fd, chunk, sizeof(chunk));
        if (n == 0) /* EOF: browser exited */
        {
            cdp_set_dead(c, 1);
            break;
        }
        if (n < 0)
        {
            if (errno == EINTR) continue;
            cdp_set_dead(c, 1);
            break;
        }

        if (cdp_buf_append(c, chunk, (size_t)n) != 0)
        {
            log_error("cdp buffer OOM, transport dead", NULL);
            cdp_set_dead(c, 1);
            break;
        }

        /* Split on NUL: the pipe protocol is one JSON message per NUL
         * byte (Chromium devtools_pipe_handler's PipeReaderASCIIZ —
         * PipeWriterASCIIZ appends '\0' after each message). Malformed
         * lines are logged and dropped; only OOM (-1) kills the
         * transport. */
        for (;;)
        {
            if (c->line_len == 0) break;
            char *nul = memchr(c->line_buf, '\0', c->line_len);
            if (!nul) break;
            size_t linelen = (size_t)(nul - c->line_buf);
            if (cdp_handle_line(c, c->line_buf) != 0)
            {
                log_error("cdp line dispatch OOM, transport dead", NULL);
                cdp_set_dead(c, 1);
                break;
            }
            memmove(c->line_buf, nul + 1, c->line_len - linelen);
            c->line_len -= linelen + 1;
        }
    }

    /* Drain whatever the browser left in the pipe before exiting so a
     * pending cdp_client_call can still observe death promptly. */
    cdp_set_dead(c, 1);
    return NULL;
}

CdpClient *cdp_client_new(int write_fd, int read_fd)
{
    /* Writing to a pipe whose reader (the browser) has exited raises
     * SIGPIPE; the transport must surface that as EPIPE/death instead
     * of killing the host process (server.c does the same). */
    (void)signal(SIGPIPE, SIG_IGN);

    CdpClient *c = calloc(1, sizeof(CdpClient));
    if (!c)
    {
        log_error("cdp client OOM", NULL);
        return NULL;
    }
    c->write_fd = write_fd;
    c->read_fd = read_fd;

    int rc = pthread_mutex_init(&c->mu, NULL);
    if (rc != 0)
    {
        free(c);
        return NULL;
    }
    rc = pthread_cond_init(&c->cv, NULL);
    if (rc != 0)
    {
        pthread_mutex_destroy(&c->mu);
        free(c);
        return NULL;
    }

    if (pthread_create(&c->reader, NULL, cdp_reader_main, c) != 0)
    {
        pthread_cond_destroy(&c->cv);
        pthread_mutex_destroy(&c->mu);
        free(c);
        return NULL;
    }
    c->started = 1;
    return c;
}

char *cdp_client_call(CdpClient *c, const char *method, cJSON *params,
                      const char *session_id, int timeout_ms)
{
    if (!c || !method) return NULL;

    pthread_mutex_lock(&c->mu);
    if (c->dead)
    {
        pthread_mutex_unlock(&c->mu);
        return NULL;
    }
    int id = c->next_id++;
    pthread_mutex_unlock(&c->mu);

    cJSON *msg = cJSON_CreateObject();
    if (!msg) return NULL;
    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddStringToObject(msg, "method", method);
    if (session_id)
        cJSON_AddStringToObject(msg, "sessionId", session_id);
    if (params && !cJSON_AddItemReferenceToObject(msg, "params", params))
    {
        cJSON_Delete(msg);
        return NULL;
    }

    char *line = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!line) return NULL;

    size_t len = strlen(line);
    char *framed = realloc(line, len + 2);
    if (!framed)
    {
        free(line);
        return NULL;
    }
    /* The pipe protocol terminates every message with a NUL byte, not
     * a newline (see the reader). */
    framed[len] = '\0';
    framed[len + 1] = '\0';

    size_t off = 0;
    while (off < len + 1)
    {
        ssize_t w = write(c->write_fd, framed + off, len + 1 - off);
        if (w > 0)
        {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        log_warn("cdp write failed, transport dead", NULL);
        cdp_set_dead(c, 1);
        free(framed);
        return NULL;
    }
    free(framed);

    int ms = timeout_ms > 0 ? timeout_ms : DEFAULT_TIMEOUT_MS;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&c->mu);
    for (;;)
    {
        if (c->dead)
        {
            pthread_mutex_unlock(&c->mu);
            return NULL;
        }
        for (size_t i = 0; i < c->pending_count; i++)
        {
            if (c->pending[i].id == id)
            {
                char *json = c->pending[i].json;
                c->pending[i].json = NULL;
                memmove(&c->pending[i], &c->pending[i + 1],
                        (c->pending_count - i - 1) * sizeof(c->pending[0]));
                c->pending_count--;
                pthread_mutex_unlock(&c->mu);
                return json;
            }
        }
        int rc = pthread_cond_timedwait(&c->cv, &c->mu, &deadline);
        if (rc == ETIMEDOUT)
        {
            log_warn("cdp call timed out", "method", method, NULL);
            pthread_mutex_unlock(&c->mu);
            return NULL;
        }
    }
}

int cdp_client_is_dead(CdpClient *c)
{
    if (!c) return 1;
    pthread_mutex_lock(&c->mu);
    int dead = c->dead;
    pthread_mutex_unlock(&c->mu);
    return dead;
}

void cdp_client_close(CdpClient *c)
{
    if (!c) return;

    pthread_mutex_lock(&c->mu);
    c->shutdown = 1;
    c->dead = 1;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);

    if (c->started) pthread_join(c->reader, NULL);

    close(c->write_fd);
    close(c->read_fd);

    for (size_t i = 0; i < c->pending_count; i++)
        free(c->pending[i].json);
    free(c->line_buf);
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->mu);
    free(c);
}
