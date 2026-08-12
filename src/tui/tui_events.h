/*
 * tui_events.h - thread-safe event ring between the TUI worker thread and
 * the UI thread, woken via a self-pipe that the UI loop polls.
 *
 * Ownership contract: tui_events_push() heap-allocates the event, copies
 * text/extra (the event owns them), and returns a pointer. The popping
 * thread takes ownership via tui_events_pop(). Round-trip events
 * (TUI_EV_ASK_USER / TUI_EV_APPROVAL) break that rule deliberately: the
 * worker keeps ownership of the returned event, waits on its condvar via
 * tui_event_wait_for_answer(), and frees it with tui_event_free() after the
 * UI thread answers it with tui_event_answer(). The UI thread must never
 * free a round-trip event.
 *
 * Depends on: pthreads.
 */

#ifndef ECHO_TUI_EVENTS_H
#define ECHO_TUI_EVENTS_H

#include <stddef.h>
#include <pthread.h>

typedef enum {
    TUI_EV_CHUNK,       /* content delta of the in-flight assistant reply */
    TUI_EV_THINK,       /* content delta inside a <think> block */
    TUI_EV_TOOL_START,  /* text = tool name, extra = arguments JSON */
    TUI_EV_TOOL_END,    /* text = tool name, extra = result or error */
    TUI_EV_TITLE,       /* text = generated session title */
    TUI_EV_ASK_USER,    /* round-trip: text = question; answer via answer */
    TUI_EV_APPROVAL,    /* round-trip: text = tool name, extra = args JSON */
    TUI_EV_RUN_DONE,    /* text = aggregated content, extra = error string */
    TUI_EV_SESSION,     /* text = session id (minted on first save) */
    TUI_EV_STATUS,      /* text = status-line message (job results, notices) */
    TUI_EV_JOB,         /* UI -> worker: text = job name, extra = argument */
    TUI_EV_CANCEL_ACK,  /* worker confirms it noticed cancellation */
    TUI_EV_QUIT         /* worker teardown signal (no payload) */
} TuiEventType;

typedef struct TuiEvent {
    TuiEventType type;
    char *text;     /* owned; NULL allowed */
    char *extra;    /* owned; NULL allowed */
    int result;     /* approval decision (0 deny / 1 allow), job result */
    char *answer;   /* round-trip: answer set by the UI thread; owned */
    int answered;   /* round-trip: 1 once tui_event_answer() ran */
    int round_trip; /* 1 for TUI_EV_ASK_USER / TUI_EV_APPROVAL */
    pthread_mutex_t lock; /* round-trip synchronization, initialized on push */
    pthread_cond_t cond;  /* round-trip synchronization, initialized on push */
} TuiEvent;

typedef struct TuiEvents TuiEvents;

/**
 * tui_events_init - allocate the event ring and its self-pipe
 * @capacity: ring capacity in events; must be >= 1.
 *
 * Creates the mutex/condvars and a pipe(2) pair used to wake the UI loop.
 * The pipe is created with O_CLOEXEC; the write end is held nonblocking so
 * a full pipe can never stall the worker.
 *
 * Return: caller-owned TuiEvents, or NULL on allocation/pipe failure with
 * errno set. Release with tui_events_destroy().
 */
TuiEvents *tui_events_init(size_t capacity);

/**
 * tui_events_destroy - release the ring and every queued event
 * @evs: ring to release, or NULL (no-op).
 *
 * Frees all still-queued events with tui_event_free(). The self-pipe fds
 * are closed. Not thread-safe: must not run while another thread is
 * pushing or popping.
 *
 * Return: void.
 */
void tui_events_destroy(TuiEvents *evs);

/**
 * tui_events_push - copy text/extra into a new event and enqueue it
 * @evs: ring to push onto; must be non-NULL.
 * @type: event kind.
 * @text: payload string, borrowed for the duration of the call; NULL
 *   leaves the field empty.
 * @extra: secondary payload string, borrowed; NULL allowed.
 *
 * Blocks while the ring is full. On allocation failure every previously
 * copied field is freed, nothing is enqueued, and NULL is returned — the
 * ring state is unchanged. Round-trip types get their mutex/condvar
 * initialized here.
 *
 * Return: pointer to the enqueued event (owned per the header contract),
 * or NULL on allocation failure. The caller may use the pointer to answer
 * the event (round-trip types only); fire-and-forget events must not be
 * touched after the call.
 */
TuiEvent *tui_events_push(TuiEvents *evs, TuiEventType type,
                          const char *text, const char *extra);

/**
 * tui_events_pop - remove the oldest event, or NULL when empty
 * @evs: ring to pop from; must be non-NULL.
 *
 * Nonblocking. The returned event is owned by the caller; release with
 * tui_event_free(), EXCEPT round-trip events, which the worker must
 * answer/consume via tui_event_wait_for_answer() and free itself.
 *
 * Return: caller-owned TuiEvent, or NULL when the ring is empty.
 */
TuiEvent *tui_events_pop(TuiEvents *evs);

/**
 * tui_events_pop_blocking - remove the oldest event, blocking when empty
 * @evs: ring to pop from; must be non-NULL.
 *
 * Used by the worker thread's job loop: waits on the ring's condvar until
 * a push arrives, so no wake-pipe traffic is needed. Ownership rules are
 * identical to tui_events_pop().
 *
 * Return: caller-owned TuiEvent (never NULL).
 */
TuiEvent *tui_events_pop_blocking(TuiEvents *evs);

/**
 * tui_event_free - release one event and its owned strings
 * @ev: event to release, or NULL (no-op).
 *
 * Frees text/extra/answer and destroys the round-trip mutex/condvar when
 * present. Must only be called by the owner per the header contract.
 *
 * Return: void.
 */
void tui_event_free(TuiEvent *ev);

/**
 * tui_event_wait_for_answer - block until the UI thread answers the event
 * @ev: round-trip event (TUI_EV_ASK_USER / TUI_EV_APPROVAL); non-NULL.
 * @answer_out: out-param receiving the answer string (caller frees), or
 *   NULL to discard. Receives NULL when the UI cancelled.
 * @result_out: out-param receiving the approval result (0/1), or NULL.
 *
 * Waits on the event's condvar, then returns the answer set by
 * tui_event_answer(). The event remains owned by the caller.
 *
 * Return: 0 on success, -1 when @ev is not a round-trip event.
 */
int tui_event_wait_for_answer(TuiEvent *ev, char **answer_out, int *result_out);

/**
 * tui_event_answer - answer a round-trip event and wake its waiter
 * @ev: round-trip event; non-NULL.
 * @answer: answer string, borrowed; NULL cancels (waiter returns NULL).
 * @result: approval decision (0 deny / 1 allow).
 *
 * Duplicates @answer into the event and signals the condvar. Calling this
 * twice on the same event is a leak/double-set bug — the UI answers each
 * modal exactly once.
 *
 * Return: 0 on success, -1 when @ev is not a round-trip event.
 */
int tui_event_answer(TuiEvent *ev, const char *answer, int result);

/**
 * tui_events_wake_fd - fd the UI loop polls for worker events
 * @evs: ring; non-NULL.
 *
 * Return: the read end of the self-pipe. Safe to call any time.
 */
int tui_events_wake_fd(const TuiEvents *evs);

/**
 * tui_events_drain_wake - consume pending wake bytes
 * @evs: ring; non-NULL.
 *
 * Called by the UI loop after the poll reports readability, before
 * popping. Safe from the UI thread only.
 *
 * Return: void.
 */
void tui_events_drain_wake(TuiEvents *evs);

/**
 * tui_events_empty - is the ring empty?
 * @evs: ring; non-NULL.
 *
 * Return: 1 when empty, 0 otherwise. Lock-free by design; used by tests
 * and the UI loop for idle detection, never for synchronization.
 */
int tui_events_empty(const TuiEvents *evs);

#endif /* ECHO_TUI_EVENTS_H */
