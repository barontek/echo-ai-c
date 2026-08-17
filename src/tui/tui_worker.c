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
#include <unistd.h>
#include <pthread.h>
#include "tui_worker.h"
#include "tui_stream.h"
#include <cjson/cJSON.h>
#include "../session/session_branch.h"
#include "../llm/factory.h"
#include "../llm/provider_models.h"
#include "../tools/registry.h"
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
    TuiStreamClassifier *cls; /* NULL when the classifier could not be
                               * allocated: everything ships as content */
} StreamCtx;

static void push_part(TuiWorker *w, const TuiStreamPart *part)
{
    if (part->len == 0) return;
    char *text = strndup(part->start, part->len);
    if (!text) return;
    (void)tui_events_push(w->ctx.evs,
                          part->kind == TUI_STREAM_PART_THINK
                              ? TUI_EV_THINK : TUI_EV_CHUNK,
                          text, NULL);
    free(text);
}

static void stream_on_chunk(const char *chunk, void *userdata)
{
    StreamCtx *sc = userdata;
    TuiWorker *w = sc->w;
    if (!chunk) return;

    TuiStreamPart parts[4];
    if (sc->cls)
    {
        int n = tui_stream_classifier_feed(sc->cls, chunk, parts, 4);
        for (int i = 0; i < n; i++)
            push_part(w, &parts[i]);
        return;
    }
    /* Degraded: no classification, ship the raw chunk as content */
    parts[0].kind = TUI_STREAM_PART_CONTENT;
    parts[0].start = chunk;
    parts[0].len = strlen(chunk);
    push_part(w, &parts[0]);
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
    /* A deny answer leaves the run alive (the model is told the tool was
     * refused and carries on), so the denial would otherwise be invisible
     * in the transcript. Surface it as the same "cancelled" error block
     * the aborted-run path emits — the event's text is empty, so the UI
     * appends the extra as an error block instead of sealing a stream. */
    if (!result)
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL, "cancelled");
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

/* Shared run-finish: flush the classifier, seal the stream with RUN_DONE
 * (cancelled/error reasons included), release the response. */
static void finish_stream_run(TuiWorker *w, LLMResponse *resp, StreamCtx *sc)
{
    if (sc->cls)
    {
        TuiStreamPart tail;
        if (tui_stream_classifier_flush(sc->cls, &tail) > 0)
            push_part(w, &tail);
        tui_stream_classifier_destroy(sc->cls);
    }
    if (resp && resp->content)
    {
        /* The chunks already streamed into the UI; RUN_DONE carries no
         * payload, it only seals the open block. */
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL, NULL);
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
}

static void job_run(TuiWorker *w, const char *input)
{
    if (!input || input[0] == '\0') return;

    atomic_store(&w->busy, 1);
    StreamCtx sc = {.w = w, .cls = tui_stream_classifier_create()};

    LLMResponse *resp = agent_run_streaming_new(w->ctx.agent, input,
                                                stream_on_chunk, &sc);
    atomic_store(&w->busy, 0);
    finish_stream_run(w, resp, &sc);

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
    /* Duplicate before freeing the old id: on allocation failure the
     * agent must keep its current session id, not lose it. */
    char *id_copy = str_dup(session_id);
    if (!id_copy)
    {
        session_free(s);
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Failed to load session.", NULL);
        return;
    }
    free(w->ctx.agent->session_id);
    w->ctx.agent->session_id = id_copy;
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
        (void)tui_events_push(w->ctx.evs, TUI_EV_MODEL, model, NULL);
    }
    else
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Model switch failed.", NULL);
    }
}

/* Resolve a provider's endpoint settings the same way startup (main.c
 * load_agent_config) and the web WS provider-switch path do: a per-provider
 * base_url override, else the provider's canonical default; the static
 * token from [providers] (OpenAI is OAuth-only and never takes one). */
static void resolve_provider_settings(const Conf *conf, const char *provider,
                                      const char **base_url_out,
                                      const char **token_out,
                                      const char **effort_out,
                                      int *num_ctx_out, int *keep_alive_out)
{
    const char *base_url = NULL;
    if (conf)
    {
        char key[96];
        snprintf(key, sizeof(key), "%s.base_url", provider);
        base_url = conf_get(conf, key);
    }
    if (!base_url) base_url = provider_default_base_url(provider);
    const char *token = strcmp(provider, "openai") == 0
        ? NULL : (conf ? conf_provider_token(conf, provider) : NULL);
    const char *effort = conf ? conf_get(conf, "agent.effort") : NULL;
    int num_ctx = conf ? conf_get_int(conf, "ollama.num_ctx", 4096) : 4096;
    int keep_alive = conf ? conf_get_int(conf, "ollama.keep_alive_secs", 120) : 120;

    *base_url_out = base_url;
    *token_out = token;
    *effort_out = effort;
    *num_ctx_out = num_ctx;
    *keep_alive_out = keep_alive;
}

static void job_provider(TuiWorker *w, const char *provider)
{
    if (!provider || !provider[0]) return;
    if (!provider_default_base_url(provider))
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Unknown provider.", NULL);
        return;
    }
    const char *base_url = NULL;
    const char *token = NULL;
    const char *effort = NULL;
    int num_ctx = 0;
    int keep_alive = 0;
    resolve_provider_settings(w->ctx.conf, provider, &base_url, &token,
                              &effort, &num_ctx, &keep_alive);

    if (agent_set_provider(w->ctx.agent, provider, base_url, token,
                           num_ctx, keep_alive, effort) == 0)
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Switched to provider: %s", provider);
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS, msg, NULL);
        (void)tui_events_push(w->ctx.evs, TUI_EV_PROVIDER, provider, NULL);
    }
    else
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Provider switch failed: %s", provider);
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS, msg, NULL);
    }
}

static void job_model_list(TuiWorker *w)
{
    /* The picker lists models for the provider currently installed on the
     * agent, resolved with the same settings the provider job applies. */
    const char *provider = w->ctx.agent->provider_name;
    if (!provider)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_MODELS, NULL, NULL);
        return;
    }
    const char *base_url = NULL;
    const char *token = NULL;
    const char *effort = NULL;
    int num_ctx = 0;
    int keep_alive = 0;
    resolve_provider_settings(w->ctx.conf, provider, &base_url, &token,
                              &effort, &num_ctx, &keep_alive);

    char **models = NULL;
    size_t count = 0U;
    int rc = provider_models_fetch_alloc(provider, base_url, token,
                                         w->ctx.agent->openai_auth,
                                         &models, &count);
    if (rc != 0 || count == 0U)
    {
        provider_models_free(models, count);
        (void)tui_events_push(w->ctx.evs, TUI_EV_MODELS, NULL, NULL);
        return;
    }

    /* Newline-join the names into the event's extra field so the picker
     * can split them back out. */
    size_t total = 1U;
    for (size_t i = 0; i < count; i++)
        total += strlen(models[i]) + 1U;
    char *joined = malloc(total);
    if (!joined)
    {
        provider_models_free(models, count);
        (void)tui_events_push(w->ctx.evs, TUI_EV_MODELS, NULL, NULL);
        return;
    }
    size_t pos = 0;
    for (size_t i = 0; i < count; i++)
    {
        pos += (size_t)snprintf(joined + pos, total - pos, "%s\n", models[i]);
    }
    provider_models_free(models, count);
    (void)tui_events_push(w->ctx.evs, TUI_EV_MODELS, NULL, joined);
    free(joined);
}

/* Split "first\x1fsecond" job args: returns a caller-owned copy of the
 * first segment and sets @second to the rest (NULL when no separator). */
static char *pair_first_dup(const char *arg, char sep, const char **second)
{
    const char *s = arg ? strchr(arg, sep) : NULL;
    if (second) *second = s ? s + 1 : NULL;
    return arg ? strndup(arg, s ? (size_t)(s - arg) : strlen(arg)) : NULL;
}

static void job_effort(TuiWorker *w, const char *effort)
{
    const char *provider = w->ctx.agent->provider_name;
    if (!provider)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "No provider installed.", NULL);
        return;
    }
    const char *want = effort && strcmp(effort, "default") == 0
                           ? NULL : effort;
    const char *base_url = NULL;
    const char *token = NULL;
    const char *cur = NULL;
    int num_ctx = 0;
    int keep_alive = 0;
    resolve_provider_settings(w->ctx.conf, provider, &base_url, &token,
                              &cur, &num_ctx, &keep_alive);
    if (agent_set_provider(w->ctx.agent, provider, base_url, token,
                           num_ctx, keep_alive, want) == 0)
    {
        char msg[160];
        snprintf(msg, sizeof(msg), "Effort set to: %s",
                 want ? want : "provider default");
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS, msg, NULL);
    }
    else
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Effort change failed.", NULL);
    }
}

static void job_delete(TuiWorker *w, const char *id)
{
    if (!w->ctx.sm)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Session persistence disabled.", NULL);
        return;
    }
    if (!id || !id[0]) return;
    if (session_manager_delete_session(w->ctx.sm, id) == 0)
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Deleted session: %s", id);
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS, msg, NULL);
    }
    else
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Session delete failed.", NULL);
    }
}

static void job_rename(TuiWorker *w, const char *arg)
{
    if (!w->ctx.sm)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Session persistence disabled.", NULL);
        return;
    }
    const char *name = NULL;
    char *id = pair_first_dup(arg, '\x1f', &name);
    if (!id || !id[0] || !name || !name[0])
    {
        free(id);
        return;
    }
    Session *s = session_manager_load_session_alloc(w->ctx.sm, id);
    if (!s)
    {
        free(id);
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Session not found.", NULL);
        return;
    }
    char *title = str_dup(name);
    if (!title)
    {
        free(id);
        session_free(s);
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Out of memory.", NULL);
        return;
    }
    free(s->title);
    s->title = title;
    if (session_manager_save_session(w->ctx.sm, s) == 0)
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Renamed session: %s", id);
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS, msg, NULL);
    }
    else
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Session rename failed.", NULL);
    }
    free(id);
    session_free(s);
}

static void job_export(TuiWorker *w, const char *arg)
{
    if (!w->ctx.sm)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Session persistence disabled.", NULL);
        return;
    }
    const char *path = NULL;
    char *id = pair_first_dup(arg, '\x1f', &path);
    if (!id || !id[0])
    {
        free(id);
        return;
    }
    char *json = session_manager_export_session_new(w->ctx.sm, id);
    if (!json)
    {
        free(id);
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Session export failed.", NULL);
        return;
    }
    const char *out_path = (path && path[0]) ? path : id;
    FILE *f = fopen(out_path, "w");
    int ok = 0;
    if (f)
    {
        ok = fputs(json, f) >= 0;
        fclose(f);
    }
    free(json);
    if (ok)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "Exported session to: %s", out_path);
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS, msg, NULL);
    }
    else
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Export write failed.", NULL);
    }
    free(id);
}

static void job_change_password(TuiWorker *w, const char *password)
{
    if (!w->ctx.sm)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Session persistence disabled.", NULL);
        return;
    }
    if (!password || !password[0]) return;
    if (migration_change_password(w->ctx.sm, password) == 0)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Password changed; all sessions re-encrypted.",
                              NULL);
    }
    else
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Password change failed.", NULL);
    }
}

static void job_openai_login(TuiWorker *w)
{
    if (!w->ctx.oauth)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "OpenAI OAuth is not available.");
        return;
    }
    char *verification_url = NULL;
    char *user_code = NULL;
    char *login_id = NULL;
    unsigned int interval = 0U;
    if (openai_oauth_device_start(w->ctx.oauth, &verification_url,
                                  &user_code, &login_id, &interval) != 0)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "OpenAI device login could not be started.");
        return;
    }
    char notice[512];
    snprintf(notice, sizeof(notice),
             "Open %s and enter code: %s", verification_url, user_code);
    free(verification_url);
    free(user_code);
    (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL, notice);

    OpenAIOAuthDeviceResult result = OPENAI_OAUTH_DEVICE_PENDING;
    int polls = 0;
    while (result == OPENAI_OAUTH_DEVICE_PENDING ||
           result == OPENAI_OAUTH_DEVICE_TRANSIENT)
    {
        sleep(interval > 0U ? interval : 1U);
        result = openai_oauth_device_poll(w->ctx.oauth, login_id);
        if (result == OPENAI_OAUTH_DEVICE_PENDING && (++polls % 6) == 0)
            (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                                  "Still waiting for OpenAI authorization \xE2\x80\xA6",
                                  NULL);
    }
    free(login_id);
    if (result == OPENAI_OAUTH_DEVICE_COMPLETE)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "OpenAI sign-in complete.");
    }
    else
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "OpenAI device login failed or expired.");
    }
}

static void job_openai_logout(TuiWorker *w)
{
    if (!w->ctx.oauth)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "OpenAI OAuth is not available.", NULL);
        return;
    }
    if (openai_oauth_logout(w->ctx.oauth) == 0)
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "OpenAI signed out.", NULL);
    else
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "OpenAI sign-out failed.", NULL);
}

/* "Lock" detaches the session store from the agent: no more saves until
 * /unlock reattaches it. The store object stays alive (owned by main.c);
 * this mirrors the web UI's lock, which requires re-auth before the store
 * is touched again. */
static void job_lock(TuiWorker *w)
{
    agent_set_session_manager(w->ctx.agent, NULL);
    registry_set_session_manager(NULL);
    (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS, "Database locked.", NULL);
    (void)tui_events_push(w->ctx.evs, TUI_EV_SESSION, "", NULL);
}

static void job_unlock(TuiWorker *w)
{
    if (!w->ctx.sm)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS,
                              "Session persistence disabled.", NULL);
        return;
    }
    agent_set_session_manager(w->ctx.agent, w->ctx.sm);
    registry_set_session_manager(w->ctx.sm);
    if (w->ctx.oauth &&
        openai_oauth_attach_session(w->ctx.oauth, w->ctx.sm) != 0)
        log_error("tui_worker: re-attaching stored OpenAI credentials failed", NULL);
    (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS, "Database unlocked.", NULL);
}

/* ---- edit / regenerate / branch switching ---- */

/* Map a 1-based user/assistant message position to the agent's in-memory
 * index (skipping the leading system messages). Returns -1 when out of
 * bounds or the message's role is not the one the operation requires. */
static int fork_message_index(TuiWorker *w, long n, const char *wanted_role)
{
    Agent *a = w->ctx.agent;
    int system_prefix = 0;
    while (system_prefix < a->messages_count &&
           strcmp(a->messages[system_prefix].role, "system") == 0)
        system_prefix++;
    long idx = (long)system_prefix + n - 1;
    if (n < 1 || idx >= (long)a->messages_count) return -1;
    if (wanted_role && a->messages[idx].role &&
        strcmp(a->messages[idx].role, wanted_role) != 0)
        return -1;
    return (int)idx;
}

/* Mirrors the web path's ws_run_fork: snapshot the pre-fork chain, truncate
 * the live array at the fork point, append the fork message (edit) or run
 * on the truncated context (regenerate), then stream the fresh reply. */
static void job_fork(TuiWorker *w, const char *arg, int regen_mode)
{
    if (!w->ctx.sm)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "Session persistence disabled; cannot fork.");
        return;
    }
    if (!w->ctx.agent->session_id)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "No active session; send a message first.");
        return;
    }
    const char *content = NULL;
    char *n_str = pair_first_dup(arg, '\x1f', &content);
    if (!n_str || !n_str[0])
    {
        free(n_str);
        return;
    }
    char *end = NULL;
    long n = strtol(n_str, &end, 10);
    int bad = (!end || *end != '\0' || n < 1);
    free(n_str); /* *end points into n_str: validate before freeing */
    if (bad)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "Invalid message number.");
        return;
    }
    int in_idx = fork_message_index(w, n, regen_mode ? "assistant" : "user");
    if (in_idx < 0)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "Message not found (user/assistant positions only).");
        return;
    }
    /* DB-side index: in-memory index minus the leading system messages
     * (they are runtime context, never persisted in the session row). */
    int system_prefix = 0;
    while (system_prefix < w->ctx.agent->messages_count &&
           strcmp(w->ctx.agent->messages[system_prefix].role, "system") == 0)
        system_prefix++;
    int db_idx = in_idx - system_prefix;

    SessionManagerForkResult fork_res;
    memset(&fork_res, 0, sizeof(fork_res));
    if (session_manager_fork_branch(w->ctx.sm, w->ctx.agent->session_id,
                                    NULL, db_idx, content, &fork_res) != 0)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "Fork failed.");
        return;
    }

    for (int i = in_idx; i < w->ctx.agent->messages_count; i++)
        message_clear(&w->ctx.agent->messages[i]);
    w->ctx.agent->messages_count = in_idx;

    int append_ok = 1;
    if (!regen_mode)
    {
        /* Edit mode: the minted fork message becomes the context tail so
         * the run answers the edited question. Regenerate drops it: the
         * model re-answers the truncated context instead. */
        Message *fork_msg = calloc(1, sizeof(Message));
        append_ok = 0;
        if (fork_msg && message_copy(fork_msg, &fork_res.fork_message) == 0)
        {
            Message *new_msgs = realloc(w->ctx.agent->messages,
                                        sizeof(Message) * (size_t)(in_idx + 1));
            if (new_msgs)
            {
                w->ctx.agent->messages = new_msgs;
                w->ctx.agent->messages[in_idx] = *fork_msg;
                w->ctx.agent->messages_count = in_idx + 1;
                append_ok = 1;
                free(fork_msg);
            }
        }
        if (!append_ok && fork_msg) message_free(fork_msg);
    }
    if (!append_ok)
    {
        message_clear(&fork_res.fork_message);
        free(fork_res.branch_id);
        free(fork_res.fork_message_id);
        free(fork_res.fork_group_id);
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "Out of memory applying the edit.");
        return;
    }

    /* Tell the UI which scrollback block to truncate before any chunk
     * arrives; edit mode also carries the replacement text. */
    char pos[32];
    snprintf(pos, sizeof(pos), "%ld", n);
    (void)tui_events_push(w->ctx.evs, TUI_EV_FORK, pos,
                          regen_mode ? NULL : content);

    atomic_store(&w->busy, 1);
    StreamCtx sc = {.w = w, .cls = tui_stream_classifier_create()};
    LLMResponse *resp = agent_run_streaming_context_new(w->ctx.agent,
                                                        stream_on_chunk, &sc);
    atomic_store(&w->busy, 0);
    finish_stream_run(w, resp, &sc);

    /* Regenerate: stamp the fresh reply with the fork group so branch_info
     * points the pill at it (best-effort; the chain still forks). */
    if (regen_mode && resp && w->ctx.agent->session_id)
    {
        char *tagged = session_manager_tag_message_new(w->ctx.sm,
                                    w->ctx.agent->session_id, db_idx,
                                    fork_res.fork_group_id);
        free(tagged);
    }

    message_clear(&fork_res.fork_message);
    free(fork_res.branch_id);
    free(fork_res.fork_message_id);
    free(fork_res.fork_group_id);
}

static void job_edit(TuiWorker *w, const char *arg)
{
    job_fork(w, arg, 0);
}

static void job_regen(TuiWorker *w, const char *arg)
{
    job_fork(w, arg, 1);
}

static void job_branch_switch(TuiWorker *w, const char *branch_id)
{
    if (!w->ctx.sm || !w->ctx.agent->session_id)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "No active session; cannot switch branches.");
        return;
    }
    if (!branch_id || !branch_id[0]) return;
    if (session_manager_switch_branch(w->ctx.sm, w->ctx.agent->session_id,
                                      branch_id) != 0)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "Branch not found.");
        return;
    }
    Session *s = session_manager_load_session_alloc(w->ctx.sm,
                                                    w->ctx.agent->session_id);
    if (!s)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "Branch load failed.");
        return;
    }
    if (w->ctx.agent->messages)
    {
        message_free_all(w->ctx.agent->messages, w->ctx.agent->messages_count);
        w->ctx.agent->messages = NULL;
        w->ctx.agent->messages_count = 0;
    }
    int swap_ok = 1;
    if (s->messages_count > 0)
    {
        w->ctx.agent->messages = calloc((size_t)s->messages_count,
                                        sizeof(Message));
        if (!w->ctx.agent->messages)
        {
            swap_ok = 0;
        }
        else
        {
            for (int i = 0; i < s->messages_count; i++)
            {
                if (message_copy(&w->ctx.agent->messages[i],
                                 &s->messages[i]) != 0)
                {
                    message_free_all(w->ctx.agent->messages, i);
                    w->ctx.agent->messages = NULL;
                    swap_ok = 0;
                    break;
                }
            }
            if (swap_ok)
                w->ctx.agent->messages_count = s->messages_count;
        }
    }
    session_free(s);

    /* Rebuild the scrollback from the branch chain (empty JSON on a
     * failed swap: the DB branch is still intact). */
    cJSON *arr = messages_to_json_array(w->ctx.agent->messages,
                                        w->ctx.agent->messages_count);
    if (arr)
    {
        char *json = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        if (json)
        {
            (void)tui_events_push(w->ctx.evs, TUI_EV_HISTORY, NULL, json);
            free(json);
        }
    }
    (void)tui_events_push(w->ctx.evs, TUI_EV_STATUS, "Switched branch.", NULL);
}

static void job_branch_info(TuiWorker *w)
{
    if (!w->ctx.sm || !w->ctx.agent->session_id)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "No active session.");
        return;
    }
    char *info = session_manager_branch_info_alloc(w->ctx.sm,
                                                   w->ctx.agent->session_id);
    if (!info)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "No branch metadata for this session.");
        return;
    }
    cJSON *root = cJSON_Parse(info);
    free(info);
    if (!root)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL,
                              "No branch metadata for this session.");
        return;
    }
    char *pretty = cJSON_Print(root);
    cJSON_Delete(root);
    if (pretty)
    {
        (void)tui_events_push(w->ctx.evs, TUI_EV_RUN_DONE, NULL, pretty);
        free(pretty);
    }
}

static void job_dispatch(TuiWorker *w, TuiEvent *ev)
{
    if (strcmp(ev->text, "run") == 0) job_run(w, ev->extra);
    else if (strcmp(ev->text, "new") == 0) job_new(w);
    else if (strcmp(ev->text, "load") == 0) job_load(w, ev->extra);
    else if (strcmp(ev->text, "model") == 0) job_model(w, ev->extra);
    else if (strcmp(ev->text, "provider") == 0) job_provider(w, ev->extra);
    else if (strcmp(ev->text, "model-list") == 0) job_model_list(w);
    else if (strcmp(ev->text, "effort") == 0) job_effort(w, ev->extra);
    else if (strcmp(ev->text, "delete") == 0) job_delete(w, ev->extra);
    else if (strcmp(ev->text, "rename") == 0) job_rename(w, ev->extra);
    else if (strcmp(ev->text, "export") == 0) job_export(w, ev->extra);
    else if (strcmp(ev->text, "change-password") == 0)
        job_change_password(w, ev->extra);
    else if (strcmp(ev->text, "openai-login") == 0) job_openai_login(w);
    else if (strcmp(ev->text, "openai-logout") == 0) job_openai_logout(w);
    else if (strcmp(ev->text, "lock") == 0) job_lock(w);
    else if (strcmp(ev->text, "unlock") == 0) job_unlock(w);
    else if (strcmp(ev->text, "edit") == 0) job_edit(w, ev->extra);
    else if (strcmp(ev->text, "regen") == 0) job_regen(w, ev->extra);
    else if (strcmp(ev->text, "branch") == 0) job_branch_switch(w, ev->extra);
    else if (strcmp(ev->text, "branch-info") == 0) job_branch_info(w);
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

        /* Drain queued jobs so a cancelled run is never followed by a
         * queued "run" job: agent_run_streaming_new() resets the cancel
         * flag, so the worker would otherwise execute full runs after
         * quit while the UI thread sits in pthread_join() and no longer
         * drains the event ring — a long run then blocks forever in
         * tui_events_push(). */
        TuiEvent *j;
        while ((j = tui_events_pop(w->ctx.jobs)) != NULL)
            tui_event_free(j);

        /* Answer any round-trip events still queued (the UI closed its
         * modal at quit and stopped popping). They stay worker-owned —
         * the worker frees them once its wait returns — so they must not
         * be freed here. This also unblocks a worker stuck waiting on a
         * condvar that no one else will ever signal. */
        TuiEvent *ev;
        while ((ev = tui_events_pop(w->ctx.evs)) != NULL)
        {
            if (ev->round_trip)
                (void)tui_event_answer(ev, NULL, 0);
            else
                tui_event_free(ev);
        }

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
