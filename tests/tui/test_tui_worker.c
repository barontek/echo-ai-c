/* test_tui_worker.c - worker-thread job tests for provider/model switching.
 * The agent and model-fetcher are stubbed (their real behavior is covered
 * in test_agent_provider.c / test_provider_models.c / routes tests); here
 * the focus is job dispatch, per-provider settings resolution from Conf,
 * and the events pushed back to the UI thread. Depends on: check,
 * tui_worker, tui_events, tui_stream, config.
 */

#define _GNU_SOURCE
#include <check.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tui/tui_worker.h"
#include "tui/tui_events.h"
#include "session/session_branch.h"
#include "llm/provider_models.h"
#include "llm/factory.h"
#include "tools/tool.h"
#include "config/config.h"
#include "utils/string_utils.h"

/* ---- stub state ---- */

static int stub_set_model_result = 0;
static const char *stub_set_model_name = NULL;
static int stub_set_provider_result = 0;
static _Atomic int stub_set_provider_calls = 0;
static const char *stub_set_provider_name = NULL;
static const char *stub_set_provider_base_url = NULL;
static const char *stub_set_provider_token = NULL;
static const char *stub_set_provider_effort = NULL;
static _Atomic int stub_set_provider_num_ctx = 0;
static _Atomic int stub_set_provider_keep_alive = 0;
static int stub_fetch_result = 0;
static size_t stub_fetch_count = 0U;
static const char *const *stub_fetch_models = NULL;
static int stub_load_session_result = 0;

/* ---- stubs: agent ---- */

void agent_cancel(Agent *agent) { (void)agent; }

void agent_destroy(Agent *agent)
{
    /* The test owns the fake agent; the worker must not free it. */
    (void)agent;
}

int agent_set_model(Agent *agent, const char *model)
{
    (void)agent;
    free((void *)stub_set_model_name);
    stub_set_model_name = model ? str_dup(model) : NULL;
    return stub_set_model_result;
}

int agent_set_provider(Agent *agent, const char *provider, const char *base_url,
                       const char *api_token, int num_ctx, int keep_alive_secs,
                       const char *effort)
{
    (void)agent;
    /* The job args live in the worker's event (freed after dispatch), so
     * the stub must copy what the test asserts on. */
    free((void *)stub_set_provider_name);
    free((void *)stub_set_provider_base_url);
    free((void *)stub_set_provider_token);
    free((void *)stub_set_provider_effort);
    stub_set_provider_calls++;
    stub_set_provider_name = provider ? str_dup(provider) : NULL;
    stub_set_provider_base_url = base_url ? str_dup(base_url) : NULL;
    stub_set_provider_token = api_token ? str_dup(api_token) : NULL;
    stub_set_provider_effort = effort ? str_dup(effort) : NULL;
    stub_set_provider_num_ctx = num_ctx;
    stub_set_provider_keep_alive = keep_alive_secs;
    return stub_set_provider_result;
}

LLMResponse *agent_run_streaming_new(Agent *agent, const char *user_input,
                                     void (*on_chunk)(const char *, void *),
                                     void *userdata)
{
    (void)agent; (void)user_input; (void)on_chunk; (void)userdata;
    return NULL;
}

void llm_response_free(LLMResponse *r) { (void)r; }

void agent_set_ask_user_callback(Agent *a, ask_user_callback cb, void *ud)
{ (void)a; (void)cb; (void)ud; }

void agent_set_session_manager(Agent *a, SessionManager *sm)
{ (void)a; (void)sm; }
void agent_set_approval_callback(Agent *a,
                                 int (*cb)(const char *, const char *, void *),
                                 void *ud)
{ (void)a; (void)cb; (void)ud; }
void agent_set_tool_start_callback(Agent *a, tool_start_callback cb, void *ud)
{ (void)a; (void)cb; (void)ud; }
void agent_set_tool_end_callback(Agent *a, tool_end_callback cb, void *ud)
{ (void)a; (void)cb; (void)ud; }
void agent_set_title_callback(Agent *a, title_callback cb, void *ud)
{ (void)a; (void)cb; (void)ud; }

/* ---- stubs: session persistence (load path only) ---- */

static Session *stub_loaded_session = NULL;
static int stub_load_has_messages = 0;
static int stub_delete_result = 0;
static int stub_save_result = 0;
static _Atomic int stub_save_calls = 0;
static Session *stub_saved_session = NULL;
static char *stub_export_json = NULL;
static int stub_change_password_result = 0;
static int stub_device_start_result = -1;

Session *session_manager_load_session_alloc(SessionManager *sm, const char *id)
{
    (void)sm; (void)id;
    if (!stub_load_session_result) return NULL;
    stub_loaded_session = calloc(1, sizeof(Session));
    if (!stub_loaded_session) return NULL;
    stub_loaded_session->title = str_dup("old title");
    if (stub_load_has_messages)
    {
        stub_loaded_session->messages_count = 2;
        stub_loaded_session->messages = calloc(2, sizeof(Message));
        if (stub_loaded_session->messages)
        {
            stub_loaded_session->messages[0].role = str_dup("user");
            stub_loaded_session->messages[0].content = str_dup("hello");
            stub_loaded_session->messages[1].role = str_dup("assistant");
            stub_loaded_session->messages[1].content = str_dup("hi there");
        }
    }
    return stub_loaded_session;
}

void session_free(Session *s) { (void)s; }

int session_manager_delete_session(SessionManager *sm, const char *id)
{
    (void)sm; (void)id;
    return stub_delete_result;
}

int session_manager_save_session(SessionManager *sm, Session *session)
{
    (void)sm;
    stub_save_calls++;
    stub_saved_session = session;
    return stub_save_result;
}

char *session_manager_export_session_new(SessionManager *sm, const char *id)
{
    (void)sm; (void)id;
    return stub_export_json ? str_dup(stub_export_json) : NULL;
}

int migration_change_password(SessionManager *sm, const char *password)
{
    (void)sm; (void)password;
    return stub_change_password_result;
}

int openai_oauth_device_start(OpenAIOAuth *auth, char **verification_url,
                              char **user_code, char **login_id,
                              unsigned int *poll_interval_seconds)
{
    (void)auth;
    if (verification_url) *verification_url = NULL;
    if (user_code) *user_code = NULL;
    if (login_id) *login_id = NULL;
    if (poll_interval_seconds) *poll_interval_seconds = 1U;
    return stub_device_start_result;
}

OpenAIOAuthDeviceResult openai_oauth_device_poll(OpenAIOAuth *auth,
                                                 const char *login_id)
{
    (void)auth; (void)login_id;
    return OPENAI_OAUTH_DEVICE_TERMINAL;
}

/* ---- stubs: message + branch APIs (fork/regen/switch jobs) ---- */

static _Atomic int stub_fork_calls = 0;
static int stub_fork_result = 0;
static const char *stub_fork_content = NULL;
static _Atomic int stub_fork_index = -1;
static int stub_switch_result = 0;
static _Atomic int stub_switch_calls = 0;
static _Atomic int stub_tag_calls = 0;
static const char *stub_branch_info = NULL;

int message_copy(Message *dst, const Message *src)
{
    if (!dst || !src) return -1;
    *dst = *src; /* shallow: fork stubs carry no heap fields */
    return 0;
}

void message_clear(Message *msg) { (void)msg; }

void message_free(Message *msg) { (void)msg; }

void message_free_all(Message *msgs, int count)
{
    (void)msgs; (void)count;
}

static int stub_summarize_result = 0;
static _Atomic int stub_summarize_calls = 0;

int agent_perform_summarization(Agent *agent, int original_count)
{
    (void)agent;
    (void)original_count;
    stub_summarize_calls++;
    return stub_summarize_result;
}

/* ---- stubs: bash tool for the shell job ---- */

static _Atomic int stub_bash_execute_calls = 0;
static const char *stub_bash_content = NULL;
static const char *stub_bash_error = NULL;

ToolResult *tool_result_create(const char *content)
{
    ToolResult *r = calloc(1, sizeof(ToolResult));
    if (!r) return NULL;
    r->content = content ? str_dup(content) : NULL;
    return r;
}

ToolResult *tool_result_error(const char *error, const char *category)
{
    ToolResult *r = calloc(1, sizeof(ToolResult));
    if (!r) return NULL;
    r->error = error ? str_dup(error) : NULL;
    r->error_category = category ? str_dup(category) : NULL;
    return r;
}

void tool_result_free(ToolResult *r)
{
    if (!r) return;
    free(r->content);
    free(r->error);
    free(r->error_category);
    free(r);
}

static ToolResult *stub_bash_execute(Tool *self, const char *args_json)
{
    (void)self;
    (void)args_json;
    stub_bash_execute_calls++;
    if (stub_bash_error)
        return tool_result_error(stub_bash_error, "exec_error");
    return tool_result_create(stub_bash_content ? stub_bash_content : "");
}

static Tool stub_bash_tool;

Tool *registry_get(const char *name)
{
    if (name && strcmp(name, "bash") == 0)
        return &stub_bash_tool;
    return NULL;
}

cJSON *messages_to_json_array(Message *msgs, int count)
{
    /* Real serialization so the load job's HISTORY event can be asserted;
     * the branch-switch test ignores HISTORY, so it stays green. */
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    for (int i = 0; i < count; i++)
    {
        cJSON *m = cJSON_CreateObject();
        if (!m)
        {
            cJSON_Delete(arr);
            return NULL;
        }
        cJSON_AddStringToObject(m, "role", msgs[i].role ? msgs[i].role : "");
        cJSON_AddStringToObject(m, "content",
                                msgs[i].content ? msgs[i].content : "");
        cJSON_AddItemToArray(arr, m);
    }
    return arr;
}

int session_manager_fork_branch(SessionManager *sm, const char *session_id,
                                const char *message_id, int index,
                                const char *new_content,
                                SessionManagerForkResult *out)
{
    (void)sm; (void)session_id; (void)message_id;
    stub_fork_calls++;
    stub_fork_index = index;
    /* The content pointer lives in the worker's event (freed after
     * dispatch); copy what the test asserts on. */
    free((void *)stub_fork_content);
    stub_fork_content = new_content ? str_dup(new_content) : NULL;
    if (stub_fork_result != 0) return -1;
    memset(out, 0, sizeof(*out));
    out->fork_group_id = str_dup("grp1");
    out->fork_message_id = str_dup("fmid");
    return 0;
}

int session_manager_switch_branch(SessionManager *sm, const char *session_id,
                                  const char *branch_id)
{
    (void)sm; (void)session_id; (void)branch_id;
    stub_switch_calls++;
    return stub_switch_result;
}

char *session_manager_tag_message_new(SessionManager *sm,
                                      const char *session_id, int index,
                                      const char *fork_group_id)
{
    (void)sm; (void)session_id; (void)index; (void)fork_group_id;
    stub_tag_calls++;
    return str_dup("tagged");
}

char *session_manager_branch_info_alloc(SessionManager *sm,
                                        const char *session_id)
{
    (void)sm; (void)session_id;
    return stub_branch_info ? str_dup(stub_branch_info) : NULL;
}

LLMResponse *agent_run_streaming_context_new(Agent *agent,
                                             void (*on_chunk)(const char *, void *),
                                             void *userdata)
{
    (void)agent; (void)on_chunk; (void)userdata;
    static LLMResponse resp;
    memset(&resp, 0, sizeof(resp));
    resp.content = (char *)"regenerated"; /* non-NULL: run "succeeded" */
    return &resp;
}

/* ---- stubs: registry / oauth wiring (lock/unlock jobs) ---- */

static _Atomic int stub_attach_session_calls = 0;

void registry_set_session_manager(SessionManager *sm) { (void)sm; }

int openai_oauth_attach_session(OpenAIOAuth *auth, SessionManager *sm)
{
    (void)auth; (void)sm;
    stub_attach_session_calls++;
    return 0;
}

int openai_oauth_logout(OpenAIOAuth *auth)
{
    (void)auth;
    return 0;
}

/* ---- stubs: model fetcher + provider metadata ---- */

int provider_models_fetch_alloc(const char *provider, const char *base_url,
                                const char *api_token, OpenAIOAuth *oauth,
                                char ***models_out, size_t *count_out)
{
    (void)provider; (void)base_url; (void)api_token; (void)oauth;
    if (!models_out || !count_out) return -1;
    *models_out = NULL;
    *count_out = 0U;
    if (stub_fetch_result != 0) return stub_fetch_result;
    if (stub_fetch_count > 0U)
    {
        char **models = calloc(stub_fetch_count, sizeof(char *));
        if (!models) return -1;
        for (size_t i = 0; i < stub_fetch_count; i++)
        {
            models[i] = str_dup(stub_fetch_models[i]);
            if (!models[i])
            {
                for (size_t j = 0; j < i; j++) free(models[j]);
                free(models);
                return -1;
            }
        }
        *models_out = models;
        *count_out = stub_fetch_count;
    }
    return 0;
}

void provider_models_free(char **models, size_t count)
{
    if (!models) return;
    for (size_t i = 0; i < count; i++) free(models[i]);
    free(models);
}

const char *provider_default_base_url(const char *name)
{
    if (!name) return NULL;
    if (strcmp(name, "ollama") == 0) return "http://localhost:11434";
    if (strcmp(name, "openai") == 0)
        return "https://chatgpt.com/backend-api/codex/responses";
    if (strcmp(name, "openai_compatible") == 0)
        return "http://localhost:1234";
    if (strcmp(name, "opencode_zen") == 0)
        return "https://opencode.ai/zen/v1";
    if (strcmp(name, "opencode_go") == 0)
        return "https://opencode.ai/zen/go/v1";
    return NULL;
}

/* ---- helpers ---- */

static void reset_stubs(void)
{
    free((void *)stub_set_model_name);
    free((void *)stub_set_provider_name);
    free((void *)stub_set_provider_base_url);
    free((void *)stub_set_provider_token);
    free((void *)stub_set_provider_effort);
    if (stub_saved_session)
    {
        free(stub_saved_session->title);
        free(stub_saved_session);
    }
    free(stub_export_json);
    if (stub_loaded_session)
    {
        for (int i = 0; i < stub_loaded_session->messages_count; i++)
        {
            free(stub_loaded_session->messages[i].role);
            free(stub_loaded_session->messages[i].content);
        }
        free(stub_loaded_session->messages);
        free(stub_loaded_session->title);
        free(stub_loaded_session);
    }
    stub_loaded_session = NULL;
    stub_load_has_messages = 0;
    stub_set_model_result = 0;
    stub_set_model_name = NULL;
    stub_set_provider_result = 0;
    stub_set_provider_calls = 0;
    stub_set_provider_name = NULL;
    stub_set_provider_base_url = NULL;
    stub_set_provider_token = NULL;
    stub_set_provider_effort = NULL;
    stub_set_provider_num_ctx = 0;
    stub_set_provider_keep_alive = 0;
    stub_fetch_result = 0;
    stub_fetch_count = 0U;
    stub_fetch_models = NULL;
    stub_load_session_result = 0;
    stub_delete_result = 0;
    stub_save_result = 0;
    stub_save_calls = 0;
    stub_saved_session = NULL;
    stub_export_json = NULL;
    stub_change_password_result = 0;
    stub_device_start_result = -1;
    stub_attach_session_calls = 0;
    stub_fork_calls = 0;
    stub_fork_result = 0;
    free((void *)stub_fork_content);
    stub_fork_content = NULL;
    stub_fork_index = -1;
    stub_switch_result = 0;
    stub_switch_calls = 0;
    stub_tag_calls = 0;
    stub_summarize_result = 0;
    stub_summarize_calls = 0;
    stub_bash_execute_calls = 0;
    stub_bash_content = NULL;
    stub_bash_error = NULL;
    memset(&stub_bash_tool, 0, sizeof(stub_bash_tool));
    stub_branch_info = NULL;
}

/* Write a temp conf file; returns the malloc'd path (caller frees). */
static char *write_conf(const char *body)
{
    char tmpl[] = "/tmp/test_tui_worker_conf_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
    size_t len = strlen(body);
    if (write(fd, body, len) != (ssize_t)len)
    {
        close(fd);
        unlink(tmpl);
        return NULL;
    }
    close(fd);
    return str_dup(tmpl);
}

typedef struct {
    TuiWorker *worker;
    TuiEvents *evs;
    TuiEvents *jobs;
    Agent *agent;   /* heap-allocated: the worker holds a pointer to it */
    Conf *conf;
    char *conf_path;
} Fixture;

static Fixture make_fixture(const char *conf_body)
{
    Fixture fx;
    memset(&fx, 0, sizeof(fx));
    fx.agent = calloc(1, sizeof(Agent));
    ck_assert_ptr_nonnull(fx.agent);
    fx.evs = tui_events_init(64);
    ck_assert_ptr_nonnull(fx.evs);
    fx.jobs = tui_events_init(64);
    ck_assert_ptr_nonnull(fx.jobs);
    fx.conf_path = write_conf(conf_body);
    ck_assert_ptr_nonnull(fx.conf_path);
    fx.conf = conf_load(fx.conf_path);
    ck_assert_ptr_nonnull(fx.conf);

    TuiWorkerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.agent = fx.agent;
    ctx.evs = fx.evs;
    ctx.jobs = fx.jobs;
    ctx.conf = fx.conf;
    ctx.oauth = (OpenAIOAuth *)(size_t)1; /* stubbed, never dereferenced */
    ctx.sm = (SessionManager *)(size_t)1; /* stubbed, never dereferenced */
    fx.worker = tui_worker_create(&ctx);
    ck_assert_ptr_nonnull(fx.worker);
    return fx;
}

static void destroy_fixture(Fixture *fx)
{
    tui_worker_destroy(fx->worker);
    tui_events_destroy(fx->evs);
    tui_events_destroy(fx->jobs);
    /* agent_destroy is stubbed, so release what the worker jobs stored
     * on the agent: session ids are heap-owned everywhere (str_dup), and
     * a load job swaps in a transcript array that must go too
     * (message_free_all is a no-op stub; the shallow copies own no heap
     * fields, but the array itself is a real allocation). */
    free(fx->agent->session_id);
    if (fx->agent->messages)
    {
        message_free_all(fx->agent->messages, fx->agent->messages_count);
        free(fx->agent->messages);
        fx->agent->messages = NULL;
        fx->agent->messages_count = 0;
    }
    free(fx->agent); /* the struct itself; fields released above */
    conf_free(fx->conf);
    unlink(fx->conf_path);
    free(fx->conf_path);
}

/* Pop events until one matches type+text (or the 2s timeout elapses).
 * The matched event is freed by the caller via tui_event_free. */
static TuiEvent *wait_for_event(Fixture *fx, TuiEventType type,
                                const char *text)
{
    for (int i = 0; i < 200; i++)
    {
        TuiEvent *ev;
        while ((ev = tui_events_pop(fx->evs)) != NULL)
        {
            if (ev->type == type &&
                (text == NULL || (ev->text && strcmp(ev->text, text) == 0)))
                return ev;
            tui_event_free(ev);
        }
        usleep(10000);
    }
    return NULL;
}

/* Some worker work happens AFTER the event that reports it (regen tags
 * the fresh reply once streaming finished, past the FORK event), so a
 * popped event does not order those stub writes. Poll the atomic counter
 * (bounded, same 2s budget as wait_for_event) before asserting on it. */
static void wait_for_stub_count(_Atomic int *counter, int expected)
{
    for (int i = 0; i < 200 && atomic_load(counter) < expected; i++)
        usleep(10000);
}

/* ---- tests ---- */

START_TEST(test_provider_job_resolves_conf_and_switches)
{
    reset_stubs();
    const char *conf =
        "[providers]\nopencode = sk-zen\n"
        "[opencode_zen]\nbase_url = https://zen.example/v1\n"
        "[agent]\neffort = high\n"
        "[ollama]\nnum_ctx = 2048\nkeep_alive_secs = 300\n";
    Fixture fx = make_fixture(conf);
    ck_assert_int_eq(tui_worker_submit(fx.worker, "provider", "opencode_zen"), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_PROVIDER, "opencode_zen");
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);

    ck_assert_int_eq(stub_set_provider_calls, 1);
    ck_assert_str_eq(stub_set_provider_name, "opencode_zen");
    ck_assert_str_eq(stub_set_provider_base_url, "https://zen.example/v1");
    ck_assert_str_eq(stub_set_provider_token, "sk-zen");
    ck_assert_str_eq(stub_set_provider_effort, "high");
    ck_assert_int_eq(stub_set_provider_num_ctx, 2048);
    ck_assert_int_eq(stub_set_provider_keep_alive, 300);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_provider_job_default_base_url_when_unconfigured)
{
    reset_stubs();
    Fixture fx = make_fixture("[providers]\nopencode = sk-go\n");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "provider", "opencode_go"), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_PROVIDER, "opencode_go");
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);

    ck_assert_str_eq(stub_set_provider_base_url,
                     "https://opencode.ai/zen/go/v1");
    ck_assert_str_eq(stub_set_provider_token, "sk-go");

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_provider_job_unknown_name_rejected)
{
    reset_stubs();
    Fixture fx = make_fixture("");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "provider", "anthropic"), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_STATUS, "Unknown provider.");
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);
    ck_assert_int_eq(stub_set_provider_calls, 0);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_provider_job_failure_keeps_old_provider)
{
    reset_stubs();
    stub_set_provider_result = -1;
    Fixture fx = make_fixture("");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "provider", "ollama"), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_STATUS,
                                  "Provider switch failed: ollama");
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);
    /* No TUI_EV_PROVIDER may follow a failed switch. */
    TuiEvent *pev = wait_for_event(&fx, TUI_EV_PROVIDER, "ollama");
    ck_assert_ptr_null(pev);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_model_list_job_joins_models)
{
    reset_stubs();
    static const char *const models[] = {"gpt-5.4", "gpt-5.3-codex"};
    stub_fetch_count = 2U;
    stub_fetch_models = models;
    Fixture fx = make_fixture("");
    fx.agent->provider_name = (char *)"openai";
    ck_assert_int_eq(tui_worker_submit(fx.worker, "model-list", NULL), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_MODELS, NULL);
    ck_assert_ptr_nonnull(ev);
    ck_assert(ev->extra && strcmp(ev->extra, "gpt-5.4\ngpt-5.3-codex\n") == 0);
    tui_event_free(ev);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_model_list_job_empty_pushes_empty)
{
    reset_stubs();
    stub_fetch_count = 0U;
    Fixture fx = make_fixture("");
    fx.agent->provider_name = (char *)"ollama";
    ck_assert_int_eq(tui_worker_submit(fx.worker, "model-list", NULL), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_MODELS, NULL);
    ck_assert_ptr_nonnull(ev);
    ck_assert_ptr_null(ev->extra);
    tui_event_free(ev);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_model_job_pushes_model_event)
{
    reset_stubs();
    Fixture fx = make_fixture("");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "model", "qwen2.5"), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_MODEL, "qwen2.5");
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_effort_job_applies_effort)
{
    reset_stubs();
    Fixture fx = make_fixture("[agent]\neffort = high\n");
    fx.agent->provider_name = (char *)"ollama";
    ck_assert_int_eq(tui_worker_submit(fx.worker, "effort", "high"), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_STATUS, "Effort set to: high");
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);
    ck_assert_int_eq(stub_set_provider_calls, 1);
    ck_assert_str_eq(stub_set_provider_name, "ollama");
    ck_assert_str_eq(stub_set_provider_effort, "high");

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_delete_job_deletes_session)
{
    reset_stubs();
    Fixture fx = make_fixture("");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "delete", "abc123"), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_STATUS, "Deleted session: abc123");
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_rename_job_renames_session)
{
    reset_stubs();
    stub_load_session_result = 1;
    Fixture fx = make_fixture("");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "rename", "abc\x1fNew Title"),
                     0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_STATUS, "Renamed session: abc");
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);
    ck_assert_int_eq(stub_save_calls, 1);
    ck_assert_ptr_nonnull(stub_saved_session);
    ck_assert_str_eq(stub_saved_session->title, "New Title");

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_export_job_writes_file)
{
    reset_stubs();
    stub_export_json = str_dup("{\"session\":1}");
    Fixture fx = make_fixture("");
    char path[] = "/tmp/test_tui_worker_export_XXXXXX";
    int fd = mkstemp(path);
    ck_assert_int_ge(fd, 0);
    close(fd);
    unlink(path); /* the job recreates it */
    char arg[256];
    snprintf(arg, sizeof(arg), "abc\x1f%s", path);
    ck_assert_int_eq(tui_worker_submit(fx.worker, "export", arg), 0);

    char expect[320];
    snprintf(expect, sizeof(expect), "Exported session to: %s", path);
    TuiEvent *ev = wait_for_event(&fx, TUI_EV_STATUS, expect);
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);

    /* The file must exist and hold the exported JSON. */
    FILE *f = fopen(path, "r");
    ck_assert_ptr_nonnull(f);
    char buf[64] = {0};
    size_t r = fread(buf, 1, sizeof(buf) - 1, f);
    buf[r] = '\0';
    fclose(f);
    unlink(path);
    ck_assert_str_eq(buf, "{\"session\":1}");

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_change_password_job)
{
    reset_stubs();
    Fixture fx = make_fixture("");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "change-password", "hunter2"),
                     0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_STATUS,
                                  "Password changed; all sessions re-encrypted.");
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_lock_unlock_jobs)
{
    reset_stubs();
    Fixture fx = make_fixture("");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "lock", NULL), 0);
    TuiEvent *ev = wait_for_event(&fx, TUI_EV_STATUS, "Database locked.");
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);

    ck_assert_int_eq(tui_worker_submit(fx.worker, "unlock", NULL), 0);
    ev = wait_for_event(&fx, TUI_EV_STATUS, "Database unlocked.");
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);
    ck_assert_int_eq(stub_attach_session_calls, 1);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_openai_login_failure_reported)
{
    reset_stubs();
    stub_device_start_result = -1;
    Fixture fx = make_fixture("");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "openai-login", NULL), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_RUN_DONE, NULL);
    ck_assert_ptr_nonnull(ev);
    ck_assert_ptr_nonnull(ev->extra);
    ck_assert(strstr(ev->extra, "could not be started") != NULL);
    tui_event_free(ev);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_openai_logout_job)
{
    reset_stubs();
    Fixture fx = make_fixture("");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "openai-logout", NULL), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_STATUS, "OpenAI signed out.");
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);

    destroy_fixture(&fx);
}

END_TEST

/* [system, user "hi", assistant "yo"]: user is #1, assistant is #2. */
static void seed_context(Fixture *fx)
{
    fx->agent->messages = calloc(3, sizeof(Message));
    ck_assert_ptr_nonnull(fx->agent->messages);
    fx->agent->messages[0].role = (char *)"system";
    fx->agent->messages[1].role = (char *)"user";
    fx->agent->messages[1].content = (char *)"hi";
    fx->agent->messages[2].role = (char *)"assistant";
    fx->agent->messages[2].content = (char *)"yo";
    fx->agent->messages_count = 3;
    fx->agent->session_id = str_dup("s1");
}

static void unseed_context(Fixture *fx)
{
    /* message_free_all is stubbed; free the seed array ourselves. */
    free(fx->agent->messages);
    fx->agent->messages = NULL;
    fx->agent->messages_count = 0;
}

START_TEST(test_edit_job_forks_and_pushes_fork_event)
{
    reset_stubs();
    Fixture fx = make_fixture("");
    seed_context(&fx);
    ck_assert_int_eq(tui_worker_submit(fx.worker, "edit", "1\x1fhello world"),
                     0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_FORK, "1");
    ck_assert_ptr_nonnull(ev);
    ck_assert(ev->extra && strcmp(ev->extra, "hello world") == 0);
    tui_event_free(ev);

    ck_assert_int_eq(stub_fork_calls, 1);
    ck_assert_int_eq(stub_fork_index, 0); /* db idx = in-memory 1 - system 1 */
    ck_assert_str_eq(stub_fork_content, "hello world");
    ck_assert_int_eq(fx.agent->messages_count, 2); /* system + fork msg */

    unseed_context(&fx);
    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_edit_job_rejects_assistant_position)
{
    reset_stubs();
    Fixture fx = make_fixture("");
    seed_context(&fx);
    ck_assert_int_eq(tui_worker_submit(fx.worker, "edit", "2\x1fnew"), 0);

    /* The fork event must never arrive for a rejected edit. */
    TuiEvent *ev = wait_for_event(&fx, TUI_EV_RUN_DONE, NULL);
    ck_assert_ptr_nonnull(ev);
    ck_assert_ptr_nonnull(ev->extra);
    ck_assert(strstr(ev->extra, "Message not found") != NULL);
    tui_event_free(ev);
    ck_assert_int_eq(stub_fork_calls, 0);

    unseed_context(&fx);
    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_regen_job_forks_and_tags)
{
    reset_stubs();
    Fixture fx = make_fixture("");
    seed_context(&fx);
    ck_assert_int_eq(tui_worker_submit(fx.worker, "regen", "2"), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_FORK, "2");
    ck_assert_ptr_nonnull(ev);
    ck_assert_ptr_null(ev->extra); /* regen carries no replacement text */
    tui_event_free(ev);

    ck_assert_int_eq(stub_fork_calls, 1);
    ck_assert_int_eq(stub_fork_index, 1); /* db idx of the assistant msg */
    ck_assert_int_eq(fx.agent->messages_count, 2); /* truncated, no append */
    wait_for_stub_count(&stub_tag_calls, 1); /* tag trails the FORK event */
    ck_assert_int_eq(stub_tag_calls, 1);

    unseed_context(&fx);
    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_regen_job_requires_active_session)
{
    reset_stubs();
    Fixture fx = make_fixture("");
    /* No session_id: the fork must be refused before touching the store. */
    ck_assert_int_eq(tui_worker_submit(fx.worker, "regen", "1"), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_RUN_DONE, NULL);
    ck_assert_ptr_nonnull(ev);
    ck_assert_ptr_nonnull(ev->extra);
    ck_assert(strstr(ev->extra, "No active session") != NULL);
    tui_event_free(ev);
    ck_assert_int_eq(stub_fork_calls, 0);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_branch_switch_job)
{
    reset_stubs();
    stub_load_session_result = 1;
    Fixture fx = make_fixture("");
    fx.agent->session_id = str_dup("s1");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "branch", "b2"), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_STATUS, "Switched branch.");
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);
    ck_assert_int_eq(stub_switch_calls, 1);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_branch_info_job_lists_metadata)
{
    reset_stubs();
    stub_branch_info = str_dup("[{\"message_id\":\"m1\"}]");
    Fixture fx = make_fixture("");
    fx.agent->session_id = str_dup("s1");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "branch-info", NULL), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_RUN_DONE, NULL);
    ck_assert_ptr_nonnull(ev);
    ck_assert_ptr_nonnull(ev->extra);
    ck_assert(strstr(ev->extra, "message_id") != NULL);
    tui_event_free(ev);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_load_job_swaps_messages_and_pushes_history)
{
    reset_stubs();
    stub_load_session_result = 1;
    stub_load_has_messages = 1;
    Fixture fx = make_fixture("");
    fx.agent->session_id = str_dup("old-id");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "load", "s123"), 0);

    /* The loaded transcript must reach the UI as a HISTORY event (the
     * chat pane is rebuilt from this JSON on session switch). */
    TuiEvent *ev = wait_for_event(&fx, TUI_EV_HISTORY, NULL);
    ck_assert_ptr_nonnull(ev);
    ck_assert_ptr_nonnull(ev->extra);
    ck_assert(strstr(ev->extra, "hello") != NULL);
    ck_assert(strstr(ev->extra, "hi there") != NULL);
    tui_event_free(ev);

    TuiEvent *sev = wait_for_event(&fx, TUI_EV_STATUS, NULL);
    ck_assert_ptr_nonnull(sev);
    ck_assert(sev->text && strstr(sev->text, "Loaded session:") != NULL);
    tui_event_free(sev);

    ck_assert_str_eq(fx.agent->session_id, "s123");
    ck_assert_int_eq(fx.agent->messages_count, 2);
    ck_assert_ptr_nonnull(fx.agent->messages);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_load_job_empty_session_still_clears_scrollback)
{
    reset_stubs();
    stub_load_session_result = 1; /* messages_count stays 0 */
    Fixture fx = make_fixture("");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "load", "s9"), 0);

    /* A session with no messages must still clear the old scrollback. */
    TuiEvent *ev = wait_for_event(&fx, TUI_EV_HISTORY, NULL);
    ck_assert_ptr_nonnull(ev);
    ck_assert_ptr_nonnull(ev->extra);
    ck_assert_str_eq(ev->extra, "[]");
    tui_event_free(ev);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_compact_job_invokes_summarizer)
{
    reset_stubs();
    stub_summarize_result = 0;
    Fixture fx = make_fixture("");
    fx.agent->session_id = str_dup("s1");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "compact", NULL), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_STATUS, "Session summarized.");
    ck_assert_ptr_nonnull(ev);
    tui_event_free(ev);
    ck_assert_int_eq(stub_summarize_calls, 1);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_compact_job_reports_skipped_on_failure)
{
    reset_stubs();
    stub_summarize_result = -1;
    Fixture fx = make_fixture("");
    fx.agent->session_id = str_dup("s1");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "compact", NULL), 0);

    TuiEvent *ev = wait_for_event(&fx, TUI_EV_STATUS, NULL);
    ck_assert_ptr_nonnull(ev);
    ck_assert(ev->text && strstr(ev->text, "Compaction skipped") != NULL);
    tui_event_free(ev);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_shell_job_runs_command_via_bash_tool)
{
    reset_stubs();
    stub_bash_tool.execute = stub_bash_execute;
    stub_bash_content = "hi from shell";
    Fixture fx = make_fixture("");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "shell", "echo hi"), 0);

    TuiEvent *start = wait_for_event(&fx, TUI_EV_TOOL_START, "shell");
    ck_assert_ptr_nonnull(start);
    ck_assert(start->extra && strstr(start->extra, "echo hi") != NULL);
    tui_event_free(start);

    TuiEvent *end = wait_for_event(&fx, TUI_EV_TOOL_END, "shell");
    ck_assert_ptr_nonnull(end);
    ck_assert(end->extra && strstr(end->extra, "hi from shell") != NULL);
    tui_event_free(end);
    ck_assert_int_eq(stub_bash_execute_calls, 1);

    destroy_fixture(&fx);
}

END_TEST

START_TEST(test_shell_job_reports_error_result)
{
    reset_stubs();
    stub_bash_tool.execute = stub_bash_execute;
    stub_bash_error = "command rejected by safety policy";
    Fixture fx = make_fixture("");
    ck_assert_int_eq(tui_worker_submit(fx.worker, "shell", "rm -rf /"), 0);

    TuiEvent *end = wait_for_event(&fx, TUI_EV_TOOL_END, "shell");
    ck_assert_ptr_nonnull(end);
    ck_assert(end->extra && strstr(end->extra, "safety policy") != NULL);
    tui_event_free(end);

    destroy_fixture(&fx);
}

END_TEST

Suite *tui_worker_suite(void)
{
    Suite *s = suite_create("tui_worker");
    TCase *tc = tcase_create("jobs");
    tcase_add_test(tc, test_provider_job_resolves_conf_and_switches);
    tcase_add_test(tc, test_provider_job_default_base_url_when_unconfigured);
    tcase_add_test(tc, test_provider_job_unknown_name_rejected);
    tcase_add_test(tc, test_provider_job_failure_keeps_old_provider);
    tcase_add_test(tc, test_model_list_job_joins_models);
    tcase_add_test(tc, test_model_list_job_empty_pushes_empty);
    tcase_add_test(tc, test_model_job_pushes_model_event);
    tcase_add_test(tc, test_effort_job_applies_effort);
    tcase_add_test(tc, test_delete_job_deletes_session);
    tcase_add_test(tc, test_rename_job_renames_session);
    tcase_add_test(tc, test_export_job_writes_file);
    tcase_add_test(tc, test_change_password_job);
    tcase_add_test(tc, test_lock_unlock_jobs);
    tcase_add_test(tc, test_openai_login_failure_reported);
    tcase_add_test(tc, test_openai_logout_job);
    tcase_add_test(tc, test_edit_job_forks_and_pushes_fork_event);
    tcase_add_test(tc, test_edit_job_rejects_assistant_position);
    tcase_add_test(tc, test_regen_job_forks_and_tags);
    tcase_add_test(tc, test_regen_job_requires_active_session);
    tcase_add_test(tc, test_branch_switch_job);
    tcase_add_test(tc, test_branch_info_job_lists_metadata);
    tcase_add_test(tc, test_load_job_swaps_messages_and_pushes_history);
    tcase_add_test(tc, test_load_job_empty_session_still_clears_scrollback);
    tcase_add_test(tc, test_compact_job_invokes_summarizer);
    tcase_add_test(tc, test_compact_job_reports_skipped_on_failure);
    tcase_add_test(tc, test_shell_job_runs_command_via_bash_tool);
    tcase_add_test(tc, test_shell_job_reports_error_result);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = tui_worker_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_ENV);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed;
}