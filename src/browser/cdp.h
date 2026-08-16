/*
 * cdp.h - Minimal Chrome DevTools Protocol transport: JSON messages
 * delimited by NUL bytes over a pipe pair (Chromium's
 * --remote-debugging-pipe ASCIIZ transport: the browser reads fd 3 /
 * writes fd 4), with a background reader thread and id-matched
 * responses. Depends on: cJSON, pthreads.
 */

#ifndef ECHO_CDP_H
#define ECHO_CDP_H

#include <pthread.h>
#include <stddef.h>

#include <cjson/cJSON.h>

/* Bounded response queue; commands are sequential per client, so 32
 * slots never legitimately fill — the cap only guards against a broken
 * peer that answers with an unbounded stream of unknown ids. */
#define CDP_MAX_PENDING 32

typedef struct {
    int id;         /* command id this response answers */
    char *json;     /* owned; full response line (caller parses) */
} CdpPending;

/**
 * CdpClient - one CDP pipe connection to a browser process.
 *
 * Created from a ready-made fd pair (see cdp_client_new); the browser
 * module wires real pipes, tests wire socketpairs. cdp_client_call is
 * safe to call from multiple threads concurrently; each call blocks
 * until its own id arrives or the timeout/death condition trips. The
 * reader thread owns line_buf; everything else is mutex-guarded.
 */
typedef struct CdpClient {
    int write_fd;        /* commands to the browser (child fd 3) */
    int read_fd;         /* messages from the browser (child fd 4) */
    int started;         /* 1 once the reader thread is running */
    pthread_t reader;    /* background reader thread */
    pthread_mutex_t mu;  /* guards everything below */
    pthread_cond_t cv;   /* signalled on response, death, or shutdown */
    int shutdown;        /* close requested; reader exits at next poll */
    int dead;            /* transport broken: EOF, write error, OOM */
    int next_id;         /* command id counter */
    CdpPending pending[CDP_MAX_PENDING];
    size_t pending_count;
    char *line_buf;      /* reader-thread only: partial line accumulator */
    size_t line_len;
    size_t line_cap;
} CdpClient;

/**
 * cdp_client_new - create a client over an existing fd pair
 * @write_fd: fd commands are written to; the client takes ownership.
 * @read_fd: fd responses/events are read from; ownership transfers.
 *
 * Return: allocated CdpClient with the reader thread running (the
 * thread starts here, so no separate start call is needed), or NULL on
 * allocation failure (fds remain open, caller owns them). Release with
 * cdp_client_close(). Does not fail for a broken fd pair: the transport
 * is simply marked dead once the read side observes EOF.
 */
CdpClient *cdp_client_new(int write_fd, int read_fd);

/**
 * cdp_client_call - send one command and wait for its response
 * @c: client; must not be used after cdp_client_close().
 * @method: CDP method name, e.g. "Browser.getVersion".
 * @params: params object attached to the command, or NULL. The message
 *   does not take ownership: the caller keeps and frees @params.
 * @session_id: CDP session id to route through (flatten mode), or NULL
 *   for browser-level commands.
 * @timeout_ms: how long to wait for the matching response; 0 selects a
 *   30 s default. A timeout does not kill the transport: the
 *   response may still arrive and is then served to no one (dropped on
 *   pending overflow), so the caller should treat timeouts as fatal for
 *   its own request sequencing.
 *
 * Return: caller-owned NUL-terminated JSON of the full response line
 * ({"id":N,"result":...} or {"id":N,"error":...}), NULL on transport
 * death, allocation failure, or timeout (check cdp_client_is_dead() to
 * tell death apart from a slow response). Parse with cJSON and free
 * with free().
 */
char *cdp_client_call(CdpClient *c, const char *method, cJSON *params,
                      const char *session_id, int timeout_ms);

/**
 * cdp_client_is_dead - has the transport failed?
 * @c: client.
 *
 * Return: 1 when the pipe saw EOF, a write failed, the buffer OOM'd, or
 * close was requested. Never fails.
 */
int cdp_client_is_dead(CdpClient *c);

/**
 * cdp_client_close - tear down the transport
 * @c: client, or NULL (no-op).
 *
 * Requests shutdown, joins the reader thread, closes both fds, frees
 * queued responses and the client itself. All fds owned by the client
 * are closed here; no other close is needed. Safe only when no other
 * thread is inside cdp_client_call().
 */
void cdp_client_close(CdpClient *c);

#ifdef CDP_TEST
/**
 * cdp_test_set_strdup_fail - arm the alloc-failure hook
 * @nth_allocation: 1-based index of the str_dup that should fail; -1
 *   disarms. Counter resets on every call.
 *
 * Test-only fault injection (see AGENTS.md).
 */
void cdp_test_set_strdup_fail(int nth_allocation);

/**
 * cdp_test_handle_line - dispatch one complete CDP message
 * @c: client.
 * @line: NUL-terminated JSON message (frame terminator stripped).
 *
 * Test/fuzz hook for the reader thread's per-message dispatch: parses
 * the message, stores responses by id, drops events. Return: 0 when
 * the message was handled or harmlessly discarded, -1 on OOM.
 */
int cdp_test_handle_line(CdpClient *c, const char *line);
#endif

#endif
