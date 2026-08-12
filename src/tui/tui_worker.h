/*
 * tui_worker.h - the worker thread that owns the Agent and executes runs
 * and jobs on it. The UI thread never touches the agent: everything goes
 * through tui_worker_submit() into the job ring, and every worker->UI
 * signal goes through the event ring. Round-trip events (ask_user,
 * approval) block the worker on their condvar until the UI answers.
 * Depends on: agent.h, tui_events.h.
 */

#ifndef ECHO_TUI_WORKER_H
#define ECHO_TUI_WORKER_H

#include "tui_events.h"
#include "../agent/agent.h"
#include "../session/session_manager.h"
#include "../config/config.h"
#include "../llm/openai_oauth.h"
#include "../safety/safety.h"

/* Builds a fresh, fully wired agent for the /new job (config loaded, oauth
 * attached, safety + session manager set). Implemented by the caller so
 * config assembly stays out of the TUI module. Returns NULL on failure. */
typedef Agent *(*tui_agent_factory_fn)(void *userdata);

/* Borrowed pointers; all must outlive the worker. The agent pointer's
 * ownership transfers to the worker on create (it may destroy/recreate
 * it via the factory for the /new job). */
typedef struct {
    Agent *agent;
    TuiEvents *evs;         /* worker -> UI event ring */
    TuiEvents *jobs;        /* UI -> worker job ring */
    SessionManager *sm;     /* borrowed; may be NULL */
    SafetyConfig *safety;   /* borrowed; attached to rebuilt agents */
    tui_agent_factory_fn agent_factory; /* for /new; may be NULL */
    void *agent_factory_userdata;
} TuiWorkerCtx;

typedef struct TuiWorker TuiWorker;

/**
 * tui_worker_create - start the worker thread
 * @ctx: context; non-NULL. The agent pointer is owned by the worker from
 *   this point on.
 *
 * Return: caller-owned TuiWorker, or NULL when the thread cannot be
 *   started (agent ownership stays with the caller in that case). Release
 *   with tui_worker_destroy().
 */
TuiWorker *tui_worker_create(const TuiWorkerCtx *ctx);

/**
 * tui_worker_destroy - stop the worker thread and release it
 * @w: worker to release, or NULL (no-op).
 *
 * Cancels any in-flight run, waits for the run loop to reach a job
 * boundary, pushes the quit event, joins the thread, and destroys the
 * agent. Pending round-trip events must not exist when this runs (the UI
 * answers or cancels outstanding modals first).
 *
 * Return: void.
 */
void tui_worker_destroy(TuiWorker *w);

/**
 * tui_worker_submit - queue a job for the worker
 * @w: worker; non-NULL.
 * @job: job name: "run" (arg = user input), "new" (reset conversation),
 *   "load" (arg = session id), "model" (arg = model name).
 * @arg: job argument, borrowed; NULL allowed.
 *
 * Jobs are refused while a run is in flight (returns -2); the UI shows
 * the refusal in the status line. "run" with an empty arg is ignored.
 *
 * Return: 0 on success, -1 on allocation failure (nothing queued), -2
 *   while a run is in flight.
 */
int tui_worker_submit(TuiWorker *w, const char *job, const char *arg);

/**
 * tui_worker_cancel - request cancellation of the in-flight run
 * @w: worker; non-NULL.
 *
 * Sets the agent's atomic cancel flag; the run loop honors it at its next
 * checkpoint (in-flight LLM calls finish first). Safe from the UI thread.
 *
 * Return: void.
 */
void tui_worker_cancel(TuiWorker *w);

/**
 * tui_worker_busy - is a run in flight?
 * @w: worker; non-NULL.
 *
 * Return: 1 while the worker is inside agent_run_streaming_new(), 0
 *   otherwise. Safe from the UI thread.
 */
int tui_worker_busy(const TuiWorker *w);

#endif /* ECHO_TUI_WORKER_H */
