/*
 * test_routes_ws_helpers.c - shared stubs and fixtures for the
 * routes_ws test binaries: ws_send capture, uv_run/uv_now
 * stand-ins, agent/session/branch/message stubs, and the per-test
 * setup/teardown. Split from test_routes_ws.c (2026-08 file-length
 * compliance). Depends on: check, the routes_ws units.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <cjson/cJSON.h>
#include <uv.h>

#include "test_routes_ws_helpers.h"

int captured_ws_send_count = 0;
char captured_ws_json[8192] = {0};

int stub_uv_now_calls = 0;

int stub_agent_run_streaming_count = 0;
LLMResponse *stub_agent_run_streaming_resp = NULL;
int stub_streaming_chunk_count = 0;
const char *stub_streaming_chunks[4] = {NULL};
WSClient *stub_close_ws = NULL;
WSChatCtx *stub_close_ctx = NULL;

int stub_agent_create_succeeds = 1;
Session *stub_session_load_result = NULL;

/* Recorded by the agent_set_provider / agent_set_model stubs below;
 * declared here so setup() can reset them. */
int stub_agent_set_provider_result = 0;
int stub_agent_set_provider_calls = 0;
const char *stub_agent_set_provider_name = NULL;
const char *stub_agent_set_provider_base_url = NULL;
const char *stub_agent_set_provider_token = NULL;
int stub_agent_set_provider_num_ctx = 0;
int stub_agent_set_provider_keep_alive = 0;
const char *stub_agent_set_provider_effort = NULL;
const char *stub_agent_set_model_name = NULL;
int stub_agent_cancel_calls = 0;
int stub_agent_clear_sm_calls = 0;
int stub_session_manager_free_calls = 0;

LLMResponse fake_resp_basic = {0};
LLMResponse fake_resp_with_tools = {0};
ToolCall fake_tools[2] = {0};
Session fake_session = {0};
Message fake_session_msgs[1] = {0};

static void reset_fork_stubs(void);
int stub_switch_rc;
const char *stub_branch_info_json;

void reset_capture(void)
{
    captured_ws_send_count = 0;
    memset(captured_ws_json, 0, sizeof(captured_ws_json));
}

void setup(void)
{
    reset_capture();
    stub_uv_now_calls = 0;
    stub_agent_run_streaming_count = 0;
    stub_agent_run_streaming_resp = NULL;
    stub_streaming_chunk_count = 0;
    stub_close_ws = NULL;
    stub_close_ctx = NULL;
    for (int i = 0; i < 4; i++) stub_streaming_chunks[i] = NULL;
    stub_agent_create_succeeds = 1;
    stub_session_load_result = NULL;
    stub_agent_set_provider_result = 0;
    stub_agent_set_provider_calls = 0;
    free((char *)stub_agent_set_provider_name);
    free((char *)stub_agent_set_provider_base_url);
    free((char *)stub_agent_set_provider_token);
    stub_agent_set_provider_name = NULL;
    stub_agent_set_provider_base_url = NULL;
    stub_agent_set_provider_token = NULL;
    stub_agent_set_provider_num_ctx = 0;
    stub_agent_set_provider_keep_alive = 0;
    free((char *)stub_agent_set_provider_effort);
    stub_agent_set_provider_effort = NULL;
    stub_agent_cancel_calls = 0;
    stub_agent_clear_sm_calls = 0;
    stub_session_manager_free_calls = 0;
    reset_fork_stubs();
    stub_switch_rc = 0;
    stub_branch_info_json = NULL;
    free((char *)stub_agent_set_model_name);
    stub_agent_set_model_name = NULL;

    fake_resp_basic.content = "Hello world";
    fake_resp_basic.thinking = NULL;
    fake_resp_basic.tool_calls = NULL;
    fake_resp_basic.tool_calls_count = 0;

    fake_tools[0].name = "search";
    fake_tools[0].arguments = "{\"q\":\"test\"}";
    fake_tools[0].result_content = "results here";
    fake_tools[0].result_error = NULL;
    fake_tools[1].name = "write_file";
    fake_tools[1].arguments = "{\"path\":\"/tmp/x\"}";
    fake_tools[1].result_content = NULL;
    fake_tools[1].result_error = "permission denied";
    fake_resp_with_tools.content = "Using tools";
    fake_resp_with_tools.thinking = NULL;
    fake_resp_with_tools.tool_calls = fake_tools;
    fake_resp_with_tools.tool_calls_count = 2;

    fake_session_msgs[0].role = "user";
    fake_session_msgs[0].content = "Hello";
    fake_session_msgs[0].id = NULL;
    fake_session_msgs[0].tool_calls = NULL;
    fake_session_msgs[0].tool_calls_count = 0;
    fake_session_msgs[0].tool_call_id = NULL;
    fake_session_msgs[0].tool_name = NULL;
    fake_session_msgs[0].error_category = NULL;
    fake_session_msgs[0].timestamp = 0;
    fake_session_msgs[0].thinking = NULL;
    fake_session.id = "test-session-123";
    fake_session.title = "Test Session";
    fake_session.title_generation_attempted = 0;
    fake_session.messages = fake_session_msgs;
    fake_session.messages_count = 1;
    fake_session.created_at = NULL;
    fake_session.metadata = NULL;
    fake_session.events = NULL;
}

void teardown(void) { reset_capture(); }

/* ---- STUBS ---- */

int ws_send_json(WSClient *ws, const char *json)
{
    (void)ws;
    if (!json) return 0;
    captured_ws_send_count++;
    size_t existing = strlen(captured_ws_json);
    size_t remain = sizeof(captured_ws_json) - existing - 1;
    if (remain > 0) strncat(captured_ws_json, json, remain);
    return 0;
}

int ws_send(WSClient *ws, const char *data, size_t len)
 {
    (void)ws;
    (void)data;
    (void)len;
    return 0;
}

WSChatCtx *g_loop_ctx = NULL;
int g_want_approval = 0;
char *g_want_answer = NULL;

int uv_run(uv_loop_t *loop, uv_run_mode mode)
{
    (void)loop; (void)mode;
    if (g_loop_ctx)
    {
        g_loop_ctx->approval_done = 1;
        g_loop_ctx->approval_result = g_want_approval;
        g_loop_ctx->ask_user_done = 1;
        if (g_want_answer)
        {
            free(g_loop_ctx->ask_user_response);
            g_loop_ctx->ask_user_response = str_dup(g_want_answer);
        }
    }
    return 0;
}

/* Monotonic-clock stand-in: first call returns START (used to compute the
 * deadline), later calls return LATER — enough to make a 1s ask_user
 * timeout elapse deterministically without real waiting. */
uint64_t uv_now(const uv_loop_t *loop)
{
    (void)loop;
    return stub_uv_now_calls++ == 0 ? UV_NOW_FIRST : UV_NOW_LATER;
}

LLMResponse *agent_run_streaming_new(Agent *agent, const char *input,
                                  void (*on_chunk)(const char *, void *),
                                  void *userdata)
{
    (void)agent; (void)input;
    stub_agent_run_streaming_count++;
    if (stub_close_ws && stub_close_ctx)
    {
        WSClient *close_ws = stub_close_ws;
        WSChatCtx *close_ctx = stub_close_ctx;
        stub_close_ws = NULL;
        stub_close_ctx = NULL;
        ws_chat_on_close(close_ws, close_ctx);
    }
    for (int i = 0; i < stub_streaming_chunk_count; i++)
        if (on_chunk) on_chunk(stub_streaming_chunks[i], userdata);
    return stub_agent_run_streaming_resp;
}

void llm_response_free(LLMResponse *resp) { (void)resp; }
void agent_cancel(Agent *a) { (void)a; stub_agent_cancel_calls++; }
void agent_destroy(Agent *a) {
    if (!a) return;
    free(a->session_id);
    message_free_all(a->messages, a->messages_count);
    free(a);
}

Agent *agent_create(const AgentConfig *cfg)
{
    (void)cfg;
    if (!stub_agent_create_succeeds) return NULL;
    return calloc(1, sizeof(Agent));
}

void agent_set_session_manager(Agent *a, SessionManager *sm)
 {
    (void)a;
    if (!sm) stub_agent_clear_sm_calls++;
}
void agent_set_approval_callback(Agent *a, int (*cb)(const char *, const char *, void *), void *u)
 {
    (void)a;
    (void)cb;
    (void)u;
}
void agent_set_title_callback(Agent *a, title_callback cb, void *u) { (void)a; (void)cb; (void)u; }
void agent_set_tool_start_callback(Agent *a, tool_start_callback cb, void *u) { (void)a; (void)cb; (void)u; }
void agent_set_tool_end_callback(Agent *a, tool_end_callback cb, void *u) { (void)a; (void)cb; (void)u; }
void agent_set_ask_user_callback(Agent *a, ask_user_callback cb, void *u)
 {
    (void)a;
    (void)cb;
    (void)u;
}
void agent_set_safety(Agent *a, SafetyConfig *s) { (void)a; (void)s; }
void agent_set_metrics(Agent *a, Metrics *m) { (void)a; (void)m; }

int agent_set_provider(Agent *a, const char *provider, const char *base_url,
                       const char *api_token, int num_ctx, int keep_alive_secs,
                       const char *effort)
{
    (void)a;
    stub_agent_set_provider_calls++;
    free((char *)stub_agent_set_provider_name);
    free((char *)stub_agent_set_provider_base_url);
    free((char *)stub_agent_set_provider_token);
    free((char *)stub_agent_set_provider_effort);
    stub_agent_set_provider_name = str_dup(provider);
    stub_agent_set_provider_base_url = str_dup(base_url);
    stub_agent_set_provider_token = str_dup(api_token);
    stub_agent_set_provider_num_ctx = num_ctx;
    stub_agent_set_provider_keep_alive = keep_alive_secs;
    stub_agent_set_provider_effort = effort ? str_dup(effort) : NULL;
    return stub_agent_set_provider_result;
}

int agent_set_model(Agent *a, const char *m)
{
    (void)a;
    free((char *)stub_agent_set_model_name);
    stub_agent_set_model_name = str_dup(m);
    return 0;
}
void agent_set_callback_manager(Agent *a, CallbackManager *m) { (void)a; (void)m; }

/* factory.c (linked for provider_default_base_url) references these
 * provider constructors, but nothing in routes_ws.c calls get_provider
 * — the agent layer is stubbed above. Stub them so the real factory
 * mapping links without dragging in live LLM clients. */
LLMProvider *ollama_provider_create(const char *b, int n, int k, const char *e)
 {
    (void)b;
    (void)n;
    (void)k;
    (void)e;
    return NULL;
}
LLMProvider *openai_compatible_provider_create(const char *b, const char *t,
                                               const char *e)
 {
    (void)b;
    (void)t;
    (void)e;
    return NULL;
}
LLMProvider *openai_provider_create(const char *b, const char *t, const char *e,
                                    OpenAIOAuth *auth)
 {
    (void)b;
    (void)t;
    (void)e;
    (void)auth;
    return NULL;
}
LLMProvider *opencode_zen_provider_create(const char *b, const char *t,
                                          const char *e)
 {
    (void)b;
    (void)t;
    (void)e;
    return NULL;
}

Session *session_manager_load_session_alloc(SessionManager *sm, const char *id)
 {
    (void)sm;
    (void)id;
    return stub_session_load_result;
}

SessionManager *session_manager_retain(SessionManager *sm) { return sm; }
void session_manager_free(SessionManager *sm)
{ if (sm) stub_session_manager_free_calls++; }

int session_manager_truncate_history(SessionManager *sm, const char *sid, int idx)
 {
    (void)sm;
    (void)sid;
    (void)idx;
    return 0;
}

/* Branch-API stubs. The real fork_branch hands the caller ownership of the
 * result's strings; mirror that so ws_run_fork's frees are balanced. */
int stub_fork_rc = 0;
const char *stub_fork_branch_id = NULL;
const char *stub_fork_message_id = NULL;
const char *stub_fork_group_id = NULL;
const char *stub_fork_content = NULL;
int stub_tag_rc = 0;
const char *stub_tag_id = NULL;

static void reset_fork_stubs(void)
{
    stub_fork_rc = 0;
    stub_fork_branch_id = NULL;
    stub_fork_message_id = NULL;
    stub_fork_group_id = NULL;
    stub_fork_content = NULL;
    stub_tag_rc = 0;
    stub_tag_id = NULL;
}

int session_manager_fork_branch(SessionManager *sm, const char *sid,
                                const char *message_id, int index,
                                const char *new_content,
                                SessionManagerForkResult *out)
{
    (void)sm; (void)sid; (void)message_id; (void)index; (void)new_content;
    memset(out, 0, sizeof(*out));
    if (stub_fork_rc != 0) return stub_fork_rc;
    out->branch_id = stub_fork_branch_id ? str_dup(stub_fork_branch_id) : NULL;
    out->fork_message_id = stub_fork_message_id ? str_dup(stub_fork_message_id) : NULL;
    out->fork_group_id = stub_fork_group_id ? str_dup(stub_fork_group_id) : NULL;
    if (stub_fork_content)
    {
        out->fork_message.role = str_dup("user");
        out->fork_message.content = str_dup(stub_fork_content);
    }
    return 0;
}

int stub_switch_rc = 0;
int session_manager_switch_branch(SessionManager *sm, const char *sid,
                                  const char *bid)
 {
    (void)sm;
    (void)sid;
    (void)bid;
    return stub_switch_rc;
}

char *session_manager_tag_message_new(SessionManager *sm, const char *sid,
                                  int index, const char *fork_group_id)
{
    (void)sm; (void)sid; (void)index; (void)fork_group_id;
    if (stub_tag_rc != 0) return NULL;
    return stub_tag_id ? str_dup(stub_tag_id) : NULL;
}

const char *stub_branch_info_json = NULL;
char *session_manager_branch_info_alloc(SessionManager *sm, const char *sid)
{
    (void)sm; (void)sid;
    return str_dup(stub_branch_info_json ? stub_branch_info_json : "[]");
}

void message_free(Message *msg)
{
    if (!msg) return;
    message_clear(msg);
    free(msg);
}

void message_clear(Message *msg)
{
    if (!msg) return;
    free(msg->role);
    free(msg->content);
    free(msg->id);
    free(msg->parent_id);
    free(msg->fork_group_id);
    free(msg->tool_call_id);
    free(msg->tool_name);
    free(msg->error_category);
    free(msg->thinking);
    free(msg->phase);
    free(msg->provider_state);
    if (msg->tool_calls)
    {
        for (int i = 0; i < msg->tool_calls_count; i++)
            tool_call_free(&msg->tool_calls[i]);
        free(msg->tool_calls);
    }
    msg->role = NULL;
    msg->content = NULL;
    msg->id = NULL;
    msg->parent_id = NULL;
    msg->fork_group_id = NULL;
    msg->tool_call_id = NULL;
    msg->tool_name = NULL;
    msg->error_category = NULL;
    msg->thinking = NULL;
    msg->phase = NULL;
    msg->provider_state = NULL;
    msg->tool_calls = NULL;
    msg->tool_calls_count = 0;
}

void message_free_all(Message *msgs, int count)
{
    for (int i = 0; i < count; i++)
        message_clear(&msgs[i]);
    free(msgs);
}

void tool_call_free(ToolCall *call)
{
    if (!call) return;
    free(call->id);
    free(call->name);
    free(call->arguments);
    free(call->result_content);
    free(call->result_error);
}

int message_copy(Message *dst, const Message *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->role = src->role ? str_dup(src->role) : NULL;
    dst->content = src->content ? str_dup(src->content) : NULL;
    dst->id = src->id ? str_dup(src->id) : NULL;
    dst->parent_id = src->parent_id ? str_dup(src->parent_id) : NULL;
    dst->fork_group_id = src->fork_group_id ? str_dup(src->fork_group_id) : NULL;
    dst->tool_call_id = src->tool_call_id ? str_dup(src->tool_call_id) : NULL;
    dst->tool_name = src->tool_name ? str_dup(src->tool_name) : NULL;
    dst->error_category = src->error_category ? str_dup(src->error_category) : NULL;
    dst->thinking = src->thinking ? str_dup(src->thinking) : NULL;
    dst->phase = src->phase ? str_dup(src->phase) : NULL;
    dst->provider_state = src->provider_state ? str_dup(src->provider_state) : NULL;
    dst->tool_calls_count = src->tool_calls_count;
    dst->tool_calls = NULL;
    return 0;
}

LLMResponse *agent_run_streaming_context_new(Agent *agent,
                                         void (*on_chunk)(const char *, void *),
                                         void *userdata)
{
    (void)agent;
    stub_agent_run_streaming_count++;
    if (stub_close_ws && stub_close_ctx)
    {
        WSClient *close_ws = stub_close_ws;
        WSChatCtx *close_ctx = stub_close_ctx;
        stub_close_ws = NULL;
        stub_close_ctx = NULL;
        ws_chat_on_close(close_ws, close_ctx);
    }
    for (int i = 0; i < stub_streaming_chunk_count; i++)
        if (on_chunk) on_chunk(stub_streaming_chunks[i], userdata);
    return stub_agent_run_streaming_resp;
}

void session_free(Session *s)
{
    if (s && s != &fake_session) free(s);
}

void registry_set_ask_user_callback(char *(*cb)(const char *, void *), void *u)
 {
    (void)cb;
    (void)u;
}

void log_error(const char *fmt, ...) { (void)fmt; }
int log_init(void) { return 0; }
void log_cleanup(void) {}
void log_set_level(int l) { (void)l; }
void log_msg(int l, const char *f, int line, const char *fmt, ...)
 {
    (void)l;
    (void)f;
    (void)line;
    (void)fmt;
}
