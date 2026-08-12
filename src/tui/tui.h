/*
 * tui.h - public entry point for the TUI mode. tui_app_* spans the whole
 * terminal lifecycle: stderr redirect, notcurses init, an optional
 * password modal, the chat UI loop, and teardown. main.c assembles the
 * dependencies (agent, session manager, registry) around it.
 * Depends on: agent, session, config, tui_events, tui_worker.
 */

#ifndef ECHO_TUI_H
#define ECHO_TUI_H

#include "tui_events.h"
#include "tui_worker.h"
#include "../agent/agent.h"
#include "../session/session_manager.h"
#include "../config/config.h"
#include "../llm/openai_oauth.h"
#include "../safety/safety.h"
#include "../change_tracker/change_tracker.h"

typedef struct TuiApp TuiApp;

/* Borrowed pointers; all must outlive the app. */
typedef struct {
    Agent *agent;
    TuiEvents *evs;    /* worker -> UI ring */
    TuiEvents *jobs;   /* UI -> worker ring */
    SessionManager *sm; /* may be NULL (persistence disabled) */
    OpenAIOAuth *oauth;
    SafetyConfig *safety;
    Conf *conf;
    ChangeTracker *ct;  /* /undo /redo; may be NULL */
    tui_agent_factory_fn agent_factory; /* for /new; may be NULL */
    void *agent_factory_userdata;
    const char *model;    /* initial status-bar model name */
    const char *provider;
    const char *session_id; /* initial session id, may be NULL */
    int tool_count;
    const char *log_path; /* where stderr was redirected (may be NULL) */
    const char *style;    /* [tui] config values, may be NULL */
    const char *density;
    const char *accent;
} TuiAppCtx;

/**
 * tui_app_create - enter the TUI: redirect stderr, init notcurses
 * @ctx: application context; non-NULL.
 *
 * Redirects stderr to <log_path> (or ~/.config/echo-ai/tui.log when NULL)
 * so JSON log lines never corrupt the alternate screen, then initializes
 * notcurses with the alternate screen and banner suppression. Does not
 * start the worker or the session manager.
 *
 * Return: caller-owned TuiApp, or NULL on failure (terminal state is
 *   already restored). Fails when no log path can be derived (log_path
 *   NULL and HOME unset). Release with tui_app_destroy().
 */
TuiApp *tui_app_create(const TuiAppCtx *ctx);

/**
 * tui_app_prompt_password - masked password modal
 * @app: app; non-NULL.
 * @prompt: modal body text, borrowed.
 *
 * Blocks until the user submits (returns the password, caller frees) or
 * presses Esc / submits empty (returns NULL = startup aborted).
 *
 * Return: caller-owned password string, or NULL when cancelled.
 */
char *tui_app_prompt_password(TuiApp *app, const char *prompt);

/**
 * tui_app_notice - modal message dismissed by any key
 * @app: app; non-NULL.
 * @title: modal title, borrowed.
 * @body: body text, borrowed.
 *
 * Blocks until any key is pressed. Used for unlock failures so the user
 * gets feedback instead of a silent retry.
 *
 * Return: 0 on success, -1 when the modal could not be shown.
 */
int tui_app_notice(TuiApp *app, const char *title, const char *body);

/**
 * tui_app_run - run the UI loop until the user quits
 * @app: app; non-NULL.
 * @sm: session manager to wire into the agent, or NULL. Borrowed for the
 *   duration of the run; the worker is created here and destroyed here.
 *
 * Starts the worker thread, renders the layout, and pumps: poll the two
 * event wake fds, drain events into the chat model/status/tool panel,
 * dispatch keys, redraw. Blocks until quit.
 *
 * Return: 0 on a clean user quit, non-zero on a fatal error.
 */
int tui_app_run(TuiApp *app, SessionManager *sm);

/**
 * tui_app_destroy - restore the terminal and release the app
 * @app: app to release, or NULL (no-op).
 *
 * Must be called with no worker running (tui_app_run returned). Restores
 * the terminal via notcurses_stop() on every path.
 *
 * Return: void.
 */
void tui_app_destroy(TuiApp *app);

#endif /* ECHO_TUI_H */
