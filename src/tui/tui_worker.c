/*
 * tui_worker.c - the agent's home thread. Runs agent_run_streaming_new()
 * with callbacks that marshal events onto the worker->UI ring, and
 * executes UI->worker jobs (run/new/load/model) against the agent. The
 * agent is owned by the worker and only touched here; agent_set_model and
 * agent recreation are non-thread-safe, which is why they are jobs.
 *
 * The ask_user/approval callbacks push a round-trip event and block on
 * its condvar; the UI answers it and the worker resumes with the result.
 * Depends on: agent, session_manager, config, tui_events.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "tui_worker.h"
#include "tui_stream.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

struct TuiWorker {
    pthread_t thread;
    TuiWorkerCtx ctx;
    _Atomic int busy;
    int thread_started;
};

/* ---- stream classification: <think> deltas vs content deltas ---- */

typedef struct {
    TuiWorker *w;
    int in_think;
} StreamCtx;

static void stream_on_chunk(const char *chunk, void *userdata)
{
    StreamCtx *sc = userdata;
    TuiWorker *w = sc->w;
    if (!chunk) return;

    /* The classifier strips markers and separator whitespace, so marker
     * deltas like "<think>\n" or "\n</think>\n\n" emit nothing and only
     * flip the state; stored blocks stay clean. */
    TuiStreamPart parts[2];
    int out_think = sc->in_think;
    int n = tui_stream_split(chunk, sc->in_think, parts, &out_think);
    sc->in_think = out_think;

    for (int i = 0; i < n; i++)
    {
        char *text = strndup(parts[i].start, parts[i].len);
        if (!text) continue;
        (void)tui_events_push(w->ctx.evs,
                              parts[i].kind == TUI_STREAM_PART_THINK
                                  ? TUI_EV_THINK : TUI_EV_CHUNK,
                              text, NULL);
        free(text);
    }
}

/* ---- agent callbacks ---- */

static char *worker_ask_user(const char *question, void *userdata)
{
    TuiWorker *w = userdata;
    TuiEvent *ev = tui_events_push(w->ctx.evs, TUI_EV_ASK_USER, question, NULL);
    if (!ev) return NULL;
    char *answer = NULL;
    (void)tui_event_wait_for_answer(ev, &answer, NULL);
    tui_event_free(ev);
    return answer; /* NULL = user cancelled; ask_user tool frees non-NULL */
}

static int worker_approval(const char *tool_name, const char *arguments,
                           void *userdata)
{
    TuiWorker *w = userdata;
    TuiEvent *ev = tui_events_push(w->ctx.evs, TUI_EV_APPROVAL,
                                   tool_name, arguments);
    if (!ev) return 0; /* deny when the modal cannot be shown */
    int result = 0;
    (void)tui_event_wait_for_answer(ev, NULL, &result);
    tui_event_free(ev);
    return result;
}

static void worker_tool_start(const char *tool_name, const char *arguments,
                              void *userdata)
{
    TuiWorker *w = userdata;
    (void)tui_events_push(w->ctx.evs, TUI_EV_TOOL_START, tool_name, arguments);
}

static void worker_tool_end(const char *tool_name, const char *tool_call_id,
                            const char *result_content, const char *result_error,
                            void *userdata)
{
    TuiWorker *w = userdata;
    (void)tool_call_id;
    /* result_error is always non-NULL ("" on success) — only an actual
     * error text may shadow the result content, not an empty string. */
    const char *display = (result_error && result_error[0] != '\0')
                              ? result_error : result_content;
    (void)tui_events_push(w->ctx.evs, TUI_EV_TOOL_END, tool_name, display);
}

static void worker_title(const char *session_id, const char *title, void *userdata)
{
    TuiWorker *w = userdata;
    (void)session_id;
    (void)tui_events_push(w->ctx.evs, TUI_EV_TITLE, title, NULL);
}

/* ---- job execution ---- */

static void job_run(TuiWorker *w, const char *input)
{
    if (!input || input[0] == '\0') return;

    atomic_store(&w->busy, 1);
    StreamCtx sc = {.w = w, .in_think = 0};

    LLMResponse *resp = agent_run_streaming_new(w->ctx.agent, input,
                                                stream_on_chunk, &sc);
    atomic_store(&w->busy, 0);

    if (resp && resp->content)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, resp->content, NULL);
    }
    else
    {
        /* The run loop resets the cancel flag at start, so a set flag here
         * means cancellation during this run; anything else is a failure
         * whose details the agent already logged. */
        const char *why = atomic_load(&w->ctx.agent->cancel_requested)
                              ? "cancelled"
                              : "error (see tui.log for details)";
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL, why);
    }
    llm_response_free(resp);

    /* The first save mints the session id; tell the status bar. */
    if (w->ctx.agent->session_id)
        (void)tui_events_push(w->ctx.evs, TUI_EV_SESSION,
                              w->ctx.agent->session_id, NULL);
}

static void job_new(TuiWorker *w)
{
    agent_destroy(w->ctx.agent);
    w->ctx.agent = NULL;

    if (!w->ctx.agent_factory)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "agent factory not configured");
        return;
    }
    w->ctx.agent = w->ctx.agent_factory(w->ctx.agent_factory_userdata);
    if (!w->ctx.agent)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "failed to recreate agent");
        return;
    }
    (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS, "Session reset.", NULL);
}

static void job_load(TuiWorker *w, const char *session_id)
{
    if (!w->ctx.sm)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Session persistence disabled.", NULL);
        return;
    }
    Session *s = session_manager_load_session_alloc(w->ctx.sm, session_id);
    if (!s)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Session not found.", NULL);
        return;
    }
    free(w->ctx.agent->session_id);
    w->ctx.agent->session_id = str_dup(session_id);
    char msg[256];
    snprintf(msg, sizeof(msg), "Loaded session: %s", s->title);
    session_free(s);
    (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS, msg, NULL);
    (void)tui_events_push(w->ctx.evs, TUI_EV_SESSION, session_id, NULL);
}

static void job_model(TuiWorker *w, const char *model)
{
    if (agent_set_model(w->ctx.agent, model) == 0)
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Switched to model: %s", model);
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS, msg, NULL);
    }
    else
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Model switch failed.", NULL);
    }
}

static void job_dispatch(TuiWorker *w, TuiEvent *ev)
{
    if (strcmp(ev->text, "run") == 0) job_run(w, ev->extra);
    else if (strcmp(ev->text, "new") == 0) job_new(w);
    else if (strcmp(ev->text, "load") == 0) job_load(w, ev->extra);
    else if (strcmp(ev->text, "model") == 0) job_model(w, ev->extra);
    else
        log_error("tui_worker: unknown job", "job", ev->text ? ev->text : "?", NULL);
}

static void *worker_main(void *arg)
{
    TuiWorker *w = arg;
    while (1)
    {
        TuiEvent *ev = tui_events_pop_blocking(w->ctx.jobs);
        if (ev->type == TUI_EV_QUIT)
        {
            tui_event_free(ev);
            break;
        }
        job_dispatch(w, ev);
        tui_event_free(ev);
    }
    return NULL;
}

TuiWorker *tui_worker_create(const TuiWorkerCtx *ctx)
{
    if (!ctx || !ctx->agent || !ctx->evs || !ctx->jobs) return NULL;

    TuiWorker *w = calloc(1, sizeof(TuiWorker));
    if (!w) return NULL;
    w->ctx = *ctx;
    atomic_store(&w->busy, 0);

    if (pthread_create(&w->thread, NULL, worker_main, w) != 0)
    {
        free(w);
        return NULL;
    }
    w->thread_started = 1;

    /* Install the marshal callbacks on the worker's own agent */
    agent_set_ask_user_callback(w->ctx.agent, worker_ask_user, w);
    agent_set_approval_callback(w->ctx.agent, worker_approval, w);
    agent_set_tool_start_callback(w->ctx.agent, worker_tool_start, w);
    agent_set_tool_end_callback(w->ctx.agent, worker_tool_end, w);
    agent_set_title_callback(w->ctx.agent, worker_title, w);
    return w;
}

void tui_worker_destroy(TuiWorker *w)
{
    if (!w) return;
    if (w->thread_started)
    {
        if (atomic_load(&w->busy)) agent_cancel(w->ctx.agent);
        TuiEvent *q = tui_events_push(w->ctx.jobs, TUI_EV_QUIT, NULL, NULL);
        if (!q)
        {
            log_error("tui_worker: quit event push failed", NULL);
        }
        pthread_join(w->thread, NULL);
    }
    agent_destroy(w->ctx.agent);
    free(w);
}

int tui_worker_submit(TuiWorker *w, const char *job, const char *arg)
{
    if (!w || !job) return -1;
    if (strcmp(job, "run") == 0 && (!arg || arg[0] == '\0')) return 0;
    if (atomic_load(&w->busy) && strcmp(job, "run") != 0) return -2;

    TuiEvent *ev = tui_events_push(w->ctx.jobs, TUI_EV_JOB, job, arg);
    return ev ? 0 : -1;
}

void tui_worker_cancel(TuiWorker *w)
{
    if (w) agent_cancel(w->ctx.agent);
}

int tui_worker_busy(const TuiWorker *w)
{
    return w ? atomic_load(&w->busy) : 0;
}
