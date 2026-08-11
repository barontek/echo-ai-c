#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include <uv.h>

#include "../src/server/server.h"
#include "../src/server/websocket.h"
#include "../src/server/routes/routes.h"
#include "../src/server/routes/routes_ws.h"
#include "../src/config/config.h"
#include "../src/agent/agent.h"
#include "../src/agent/message.h"
#include "../src/session/session.h"
#include "../src/session/session_manager.h"
#include "../src/session/session_branch.h"
#include "../src/utils/string_utils.h"

/* test_routes_ws - unit tests for routes ws. Depends on: check, the module under test. */
/* WSChatCtx mirror — matches routes_ws.c */
typedef struct QueuedMsg { char *data; struct QueuedMsg *next; } QueuedMsg;
typedef struct WSChatCtx {
    Agent *agent; SessionManager *sm; SafetyConfig *safety; WSClient *ws;
    uv_loop_t *loop; char *pending_request_id; int approval_done;
    int approval_result; int ready; QueuedMsg *msg_queue;
    QueuedMsg *msg_queue_tail; char *active_session_id;
    int session_start_emitted; int ask_user_done; char *ask_user_response;
    int ask_user_timeout;
    const char *base_url; const char *api_token;
    int num_ctx; int keep_alive_secs;
    char *effort;
    int active_runs; int closing;
    ServerContext *server_ctx; unsigned long auth_generation;
    struct WSChatCtx *next;
} WSChatCtx;

extern void ws_chat_on_chunk(const char *, void *);
extern void ws_send_done(WSClient *, const char *, const char *, LLMResponse *);
extern void ws_chat_flush_queue(WSChatCtx *);
extern void ws_chat_enqueue(WSChatCtx *, const char *);
extern void ws_chat_on_message(WSClient *, const char *, size_t, void *);
extern void ws_title_update_cb(const char *, const char *, void *);
extern void ws_tool_start_cb(const char *, const char *, void *);
extern void ws_tool_end_cb(const char *, const char *, const char *, const char *, void *);
extern void ws_chat_on_close(WSClient *, void *);
extern int ws_approval_cb(const char *, const char *, void *);
extern char *ws_ask_user_cb(const char *, void *);
extern void ws_chat_emit_session_start(WSChatCtx *);

static int captured_ws_send_count = 0;
static char captured_ws_json[8192] = {0};

/* Monotonic-clock stand-in: first call returns UV_NOW_FIRST (used to
 * compute the deadline), later calls return UV_NOW_LATER — enough to
 * make a 1s ask_user timeout elapse deterministically without real
 * waiting. Declared here so setup() can reset it. */
#define UV_NOW_FIRST 1000
#define UV_NOW_LATER 2001
static int stub_uv_now_calls = 0;

static int stub_agent_run_streaming_count = 0;
static LLMResponse *stub_agent_run_streaming_resp = NULL;
static int stub_streaming_chunk_count = 0;
static const char *stub_streaming_chunks[4] = {NULL};
static WSClient *stub_close_ws = NULL;
static WSChatCtx *stub_close_ctx = NULL;

static int stub_agent_create_succeeds = 1;
static Session *stub_session_load_result = NULL;

/* Recorded by the agent_set_provider / agent_set_model stubs below;
 * declared here so setup() can reset them. */
static int stub_agent_set_provider_result = 0;
static int stub_agent_set_provider_calls = 0;
static const char *stub_agent_set_provider_name = NULL;
static const char *stub_agent_set_provider_base_url = NULL;
static const char *stub_agent_set_provider_token = NULL;
static int stub_agent_set_provider_num_ctx = 0;
static int stub_agent_set_provider_keep_alive = 0;
static const char *stub_agent_set_provider_effort = NULL;
static const char *stub_agent_set_model_name = NULL;
static int stub_agent_cancel_calls = 0;
static int stub_agent_clear_sm_calls = 0;
static int stub_session_manager_free_calls = 0;

static LLMResponse fake_resp_basic = {0};
static LLMResponse fake_resp_with_tools = {0};
static ToolCall fake_tools[2] = {0};
static Session fake_session = {0};
static Message fake_session_msgs[1] = {0};

static void reset_fork_stubs(void);
static int stub_switch_rc;
static const char *stub_branch_info_json;

static void reset_capture(void)
{
    captured_ws_send_count = 0;
    memset(captured_ws_json, 0, sizeof(captured_ws_json));
}

static void setup(void)
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

static void teardown(void) { reset_capture(); }

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

static WSChatCtx *g_loop_ctx = NULL;
static int g_want_approval = 0;
static char *g_want_answer = NULL;

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
static int stub_fork_rc = 0;
static const char *stub_fork_branch_id = NULL;
static const char *stub_fork_message_id = NULL;
static const char *stub_fork_group_id = NULL;
static const char *stub_fork_content = NULL;
static int stub_tag_rc = 0;
static const char *stub_tag_id = NULL;

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

static int stub_switch_rc = 0;
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

static const char *stub_branch_info_json = NULL;
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

/* ==================================================================
 * ws_chat_on_chunk tests
 * ================================================================== */

START_TEST(test_on_chunk_forwards_content_frame)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    ws_chat_on_chunk("hello", &c);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"type\":\"content\""));
    ck_assert(strstr(captured_ws_json, "\"content\":\"hello\""));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_edit_disconnect_during_run_is_safe)
{
    WSClient ws = {0};
    WSChatCtx *c = calloc(1, sizeof(WSChatCtx));
    Agent *agent = calloc(1, sizeof(Agent));
    ck_assert_ptr_nonnull(c);
    ck_assert_ptr_nonnull(agent);
    agent->session_id = str_dup("edit-close");
    c->agent = agent;
    c->sm = (SessionManager *)c;
    c->ws = &ws;
    ws.on_close = ws_chat_on_close;
    ws.userdata = c;
    stub_close_ws = &ws;
    stub_close_ctx = c;

    ws_chat_on_message(&ws,
        "{\"type\":\"edit\",\"index\":0,\"content\":\"replacement\"}",
        56, c);
    ck_assert_int_eq(stub_agent_run_streaming_count, 1);
    ck_assert_ptr_null(ws.userdata);
}
END_TEST

START_TEST(test_on_chunk_null_chunk)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    ws_chat_on_chunk(NULL, &c);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"content\":\"\""));
    reset_capture();
}
END_TEST

START_TEST(test_on_chunk_null_ctx)
{
    ws_chat_on_chunk("x", NULL);
    ck_assert_int_eq(captured_ws_send_count, 0);
    reset_capture();
}
END_TEST

START_TEST(test_on_chunk_null_ws)
{
    WSChatCtx c = {0};
    c.ws = NULL;
    ws_chat_on_chunk("x", &c);
    ck_assert_int_eq(captured_ws_send_count, 0);
    reset_capture();
}
END_TEST

START_TEST(test_on_chunk_with_session_id)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    c.active_session_id = "sess-1";
    ws_chat_on_chunk("hi", &c);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"session_id\":\"sess-1\""));
    reset_capture();
}
END_TEST

/* ==================================================================
 * ws_send_done tests
 * ================================================================== */

START_TEST(test_send_done_emits_done_frame)
{
    char dummy_ws = 0;
    ws_send_done((WSClient *)&dummy_ws, NULL, NULL, &fake_resp_basic);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"type\":\"done\""));
    ck_assert(strstr(captured_ws_json, "\"content\":\"Hello world\""));
    ck_assert(strstr(captured_ws_json, "\"has_tools\":false"));
    reset_capture();
}
END_TEST

START_TEST(test_send_done_null_resp)
{
    char dummy_ws = 0;
    ws_send_done((WSClient *)&dummy_ws, NULL, NULL, NULL);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"type\":\"done\""));
    reset_capture();
}
END_TEST

START_TEST(test_send_done_with_session_id)
{
    char dummy_ws = 0;
    ws_send_done((WSClient *)&dummy_ws, "abc123", NULL, &fake_resp_basic);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"session_id\":\"abc123\""));
    reset_capture();
}
END_TEST

START_TEST(test_send_done_with_title)
{
    char dummy_ws = 0;
    ws_send_done((WSClient *)&dummy_ws, NULL, "My Session", &fake_resp_basic);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"title\":\"My Session\""));
    reset_capture();
}
END_TEST

START_TEST(test_send_done_with_tool_calls)
{
    char dummy_ws = 0;
    ws_send_done((WSClient *)&dummy_ws, NULL, NULL, &fake_resp_with_tools);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"has_tools\":true"));
    ck_assert(strstr(captured_ws_json, "\"tool_calls\""));
    ck_assert(strstr(captured_ws_json, "\"name\":\"search\""));
    ck_assert(strstr(captured_ws_json, "\"result_content\":\"results here\""));
    ck_assert(strstr(captured_ws_json, "\"result_error\":\"permission denied\""));
    reset_capture();
}
END_TEST

/* ==================================================================
 * ws_chat_emit_session_start tests
 * ================================================================== */

START_TEST(test_emit_session_start_no_session_id)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    Agent agent = {0};
    agent.session_id = NULL;
    c.ws = (WSClient *)&dummy_ws;
    c.agent = &agent;
    ws_chat_emit_session_start(&c);
    ck_assert_int_eq(captured_ws_send_count, 0);
    reset_capture();
}
END_TEST

START_TEST(test_emit_session_start_with_session)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    Agent agent = {0};
    agent.session_id = "sess-42";
    c.ws = (WSClient *)&dummy_ws;
    c.agent = &agent;
    ws_chat_emit_session_start(&c);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"type\":\"session_start\""));
    ck_assert(strstr(captured_ws_json, "\"session_id\":\"sess-42\""));
    ck_assert_int_eq(c.session_start_emitted, 1);
    reset_capture();
}
END_TEST

/* ==================================================================
 * ws_title_update_cb tests
 * ================================================================== */

START_TEST(test_title_update_cb_valid)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    ws_title_update_cb("sid-1", "New Title", &c);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"type\":\"title_updated\""));
    ck_assert(strstr(captured_ws_json, "\"session_id\":\"sid-1\""));
    ck_assert(strstr(captured_ws_json, "\"title\":\"New Title\""));
    reset_capture();
}
END_TEST

START_TEST(test_title_update_cb_null_ctx)
{
    ws_title_update_cb("x", "y", NULL);
    ck_assert_int_eq(captured_ws_send_count, 0);
    reset_capture();
}
END_TEST

START_TEST(test_title_update_cb_null_ws)
{
    WSChatCtx c = {0};
    c.ws = NULL;
    ws_title_update_cb("x", "y", &c);
    ck_assert_int_eq(captured_ws_send_count, 0);
    reset_capture();
}
END_TEST

START_TEST(test_title_update_cb_nulls)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    ws_title_update_cb(NULL, NULL, &c);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"session_id\":\"\""));
    ck_assert(strstr(captured_ws_json, "\"title\":\"\""));
    reset_capture();
}
END_TEST

/* ==================================================================
 * ws_tool_start_cb tests
 * ================================================================== */

START_TEST(test_tool_start_cb_valid)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    ws_tool_start_cb("bash", "{\"cmd\":\"ls\"}", &c);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"type\":\"tool_start\""));
    ck_assert(strstr(captured_ws_json, "\"tool_name\":\"bash\""));
    reset_capture();
}
END_TEST

START_TEST(test_tool_start_cb_nulls)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    ws_tool_start_cb(NULL, NULL, &c);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"tool_name\":\"\""));
    ck_assert(strstr(captured_ws_json, "\"arguments\":\"{}\""));
    reset_capture();
}
END_TEST

START_TEST(test_tool_start_cb_null_ws)
{
    WSChatCtx c = {0};
    c.ws = NULL;
    ws_tool_start_cb("x", "y", &c);
    ck_assert_int_eq(captured_ws_send_count, 0);
    reset_capture();
}
END_TEST

/* ==================================================================
 * ws_tool_end_cb tests
 * ================================================================== */

START_TEST(test_tool_end_cb_valid)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    ws_tool_end_cb("bash", "tc-1", "output", NULL, &c);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"type\":\"tool_end\""));
    ck_assert(strstr(captured_ws_json, "\"tool_name\":\"bash\""));
    ck_assert(strstr(captured_ws_json, "\"tool_call_id\":\"tc-1\""));
    ck_assert(strstr(captured_ws_json, "\"result_content\":\"output\""));
    ck_assert(strstr(captured_ws_json, "\"result_error\":\"\""));
    reset_capture();
}
END_TEST

START_TEST(test_tool_end_cb_nulls)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    ws_tool_end_cb(NULL, NULL, NULL, NULL, &c);
    ck_assert_int_eq(captured_ws_send_count, 1);
    ck_assert(strstr(captured_ws_json, "\"tool_name\":\"\""));
    ck_assert(strstr(captured_ws_json, "\"tool_call_id\":\"\""));
    ck_assert(strstr(captured_ws_json, "\"result_content\":\"\""));
    ck_assert(strstr(captured_ws_json, "\"result_error\":\"\""));
    reset_capture();
}
END_TEST

/* ==================================================================
 * ws_chat_enqueue tests
 * ================================================================== */

START_TEST(test_enqueue_single)
{
    WSChatCtx *c = calloc(1, sizeof(WSChatCtx));
    ck_assert_ptr_nonnull(c);
    ws_chat_enqueue(c, "{\"message\":\"hi\"}");
    ck_assert_ptr_nonnull(c->msg_queue);
    ck_assert_str_eq(c->msg_queue->data, "{\"message\":\"hi\"}");
    ck_assert_ptr_eq(c->msg_queue_tail, c->msg_queue);
    ck_assert_ptr_null(c->msg_queue->next);
    WSClient dummy = {0};
    ws_chat_on_close(&dummy, c);
    reset_capture();
}
END_TEST

START_TEST(test_enqueue_multiple)
{
    WSChatCtx *c = calloc(1, sizeof(WSChatCtx));
    ck_assert_ptr_nonnull(c);
    ws_chat_enqueue(c, "{\"msg\":1}");
    ws_chat_enqueue(c, "{\"msg\":2}");
    ws_chat_enqueue(c, "{\"msg\":3}");
    ck_assert_ptr_nonnull(c->msg_queue);
    ck_assert_str_eq(c->msg_queue->data, "{\"msg\":1}");
    ck_assert_ptr_nonnull(c->msg_queue->next);
    ck_assert_str_eq(c->msg_queue->next->data, "{\"msg\":2}");
    ck_assert_str_eq(c->msg_queue_tail->data, "{\"msg\":3}");
    WSClient dummy = {0};
    ws_chat_on_close(&dummy, c);
    reset_capture();
}
END_TEST

/* ws_chat_enqueue allocates the node (calloc) then the data copy
 * (str_dup) before committing to the queue; a failure at either step
 * must leave the queue exactly as it was (no partial tail or dangling
 * node) and free the intermediate allocation (ASan-verified). */
START_TEST(test_enqueue_allocation_failure_leaves_queue_unchanged)
{
    WSChatCtx *c = calloc(1, sizeof(WSChatCtx));
    ck_assert_ptr_nonnull(c);
    ws_chat_enqueue(c, "{\"msg\":0}");
    QueuedMsg *head_before = c->msg_queue;
    QueuedMsg *tail_before = c->msg_queue_tail;
    size_t queue_len_before = 1;

    for (int fail_at = 1; fail_at <= 2; fail_at++)
    {
        routes_ws_test_set_alloc_fail(fail_at);
        ws_chat_enqueue(c, "{\"msg\":N}");
        ck_assert_ptr_eq(c->msg_queue, head_before);
        ck_assert_ptr_eq(c->msg_queue_tail, tail_before);
        ck_assert_ptr_null(tail_before->next);
        ck_assert_str_eq(c->msg_queue->data, "{\"msg\":0}");
    }
    routes_ws_test_set_alloc_fail(-1);

    /* reset: the queue still enqueues and the node chain stays intact */
    ws_chat_enqueue(c, "{\"msg\":2}");
    QueuedMsg *q = c->msg_queue;
    size_t len = 0;
    while (q) {
        len++;
        q = q->next;
    }
    ck_assert_uint_eq(len, queue_len_before + 1);
    ck_assert_str_eq(c->msg_queue_tail->data, "{\"msg\":2}");

    WSClient dummy = {0};
    ws_chat_on_close(&dummy, c);
    reset_capture();
}
END_TEST

/* ==================================================================
 * ws_chat_flush_queue tests
 * ================================================================== */

START_TEST(test_flush_queue_not_ready)
{
    WSChatCtx c = {0};
    c.ready = 1;
    ws_chat_flush_queue(&c);
    ck_assert_int_eq(captured_ws_send_count, 0);
    reset_capture();
}
END_TEST

START_TEST(test_flush_queue_empty)
{
    WSChatCtx c = {0};
    c.ready = 0;
    ws_chat_flush_queue(&c);
    ck_assert_int_eq(c.ready, 1);
    ck_assert_int_eq(captured_ws_send_count, 0);
    reset_capture();
}
END_TEST

START_TEST(test_flush_queue_single_success)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    Agent agent = {0};
    agent.session_id = NULL;
    c.agent = &agent;
    c.ws = (WSClient *)&dummy_ws;
    c.ready = 0;
    ws_chat_enqueue(&c, "{\"content\":\"hi\"}");
    stub_agent_run_streaming_resp = &fake_resp_basic;
    stub_streaming_chunk_count = 1;
    stub_streaming_chunks[0] = "\"chunk1\"";
    ws_chat_flush_queue(&c);
    ck_assert_int_eq(c.ready, 1);
    ck_assert_int_eq(stub_agent_run_streaming_count, 1);
    ck_assert(strstr(captured_ws_json, "\"type\":\"done\""));
    ck_assert_ptr_null(c.msg_queue);
    reset_capture();
}
END_TEST

START_TEST(test_flush_queue_single_null_resp)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    Agent agent = {0};
    c.agent = &agent;
    c.ws = (WSClient *)&dummy_ws;
    c.ready = 0;
    ws_chat_enqueue(&c, "{\"content\":\"hi\"}");
    stub_agent_run_streaming_resp = NULL;
    ws_chat_flush_queue(&c);
    ck_assert_int_eq(c.ready, 1);
    ck_assert(strstr(captured_ws_json, "\"type\":\"error\""));
    ck_assert(strstr(captured_ws_json, "no response"));
    ck_assert_ptr_null(c.msg_queue);
    reset_capture();
}
END_TEST

START_TEST(test_flush_queue_agent_gets_session_id)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    Agent agent = {0};
    agent.session_id = "new-sess";
    c.agent = &agent;
    c.ws = (WSClient *)&dummy_ws;
    c.active_session_id = NULL;
    c.ready = 0;
    ws_chat_enqueue(&c, "{\"content\":\"hi\"}");
    stub_agent_run_streaming_resp = &fake_resp_basic;
    ws_chat_flush_queue(&c);
    ck_assert_str_eq(c.active_session_id, "new-sess");
    reset_capture();
    free(c.active_session_id);
    c.active_session_id = NULL;
}
END_TEST

/* ==================================================================
 * ws_chat_on_close tests
 * ================================================================== */

START_TEST(test_on_close_cleanup)
{
    WSChatCtx *c = calloc(1, sizeof(WSChatCtx));
    c->agent = NULL;
    ws_chat_enqueue(c, "{\"x\":1}");
    c->pending_request_id = str_dup("req-1");
    c->active_session_id = str_dup("sess-1");
    WSClient ws = {0};
    ws.on_close = (ws_close_handler)0x1;
    ws.userdata = (void *)0x1;
    ws_chat_on_close(&ws, c);
    ck_assert(ws.on_close == NULL);
    ck_assert_ptr_null(ws.userdata);
    reset_capture();
}
END_TEST

START_TEST(test_on_close_with_agent)
{
    WSChatCtx *c = calloc(1, sizeof(WSChatCtx));
    c->agent = calloc(1, sizeof(Agent));
    WSClient ws = {0};
    ws_chat_on_close(&ws, c);
    reset_capture();
}
END_TEST

/* ==================================================================
 * ws_approval_cb tests
 * ================================================================== */

START_TEST(test_approval_cb_null_ctx)
{
    int r = ws_approval_cb("bash", "{}", NULL);
    ck_assert_int_eq(r, 0);
    reset_capture();
}
END_TEST

START_TEST(test_approval_cb_null_ws)
{
    WSChatCtx c = {0};
    c.ws = NULL;
    int r = ws_approval_cb("bash", "{}", &c);
    ck_assert_int_eq(r, 0);
    reset_capture();
}
END_TEST

START_TEST(test_approval_cb_approved)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    c.loop = (uv_loop_t *)&c;
    g_loop_ctx = &c;
    g_want_approval = 1;
    int r = ws_approval_cb("bash", "{\"cmd\":\"ls\"}", &c);
    g_loop_ctx = NULL;
    ck_assert_int_eq(r, 1);
    ck_assert_int_ge(captured_ws_send_count, 2);
    ck_assert(strstr(captured_ws_json, "\"type\":\"approval_request\""));
    ck_assert(strstr(captured_ws_json, "\"type\":\"approval_response\""));
    ck_assert(strstr(captured_ws_json, "\"approved\":true"));
    free(c.pending_request_id);
    reset_capture();
}
END_TEST

START_TEST(test_approval_cb_rejected)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    c.loop = (uv_loop_t *)&c;
    g_loop_ctx = &c;
    g_want_approval = 0;
    int r = ws_approval_cb("rm", "{}", &c);
    g_loop_ctx = NULL;
    ck_assert_int_eq(r, 0);
    ck_assert(strstr(captured_ws_json, "\"approved\":false"));
    free(c.pending_request_id);
    reset_capture();
}
END_TEST

/* ==================================================================
 * ws_ask_user_cb tests
 * ================================================================== */

START_TEST(test_ask_user_cb_null_ctx)
{
    char *r = ws_ask_user_cb("question?", NULL);
    ck_assert_ptr_null(r);
    reset_capture();
}
END_TEST

START_TEST(test_ask_user_cb_null_ws)
{
    WSChatCtx c = {0};
    c.ws = NULL;
    char *r = ws_ask_user_cb("q?", &c);
    ck_assert_ptr_null(r);
    reset_capture();
}
END_TEST

START_TEST(test_ask_user_cb_with_answer)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    c.loop = (uv_loop_t *)&c;
    g_loop_ctx = &c;
    g_want_answer = "yes, proceed";
    char *r = ws_ask_user_cb("Proceed?", &c);
    g_loop_ctx = NULL;
    g_want_answer = NULL;
    ck_assert_ptr_nonnull(r);
    ck_assert_str_eq(r, "yes, proceed");
    ck_assert(strstr(captured_ws_json, "\"type\":\"ask_user\""));
    ck_assert(strstr(captured_ws_json, "\"question\":\"Proceed?\""));
    free(r);
    free(c.ask_user_response);
    reset_capture();
}
END_TEST

START_TEST(test_ask_user_cb_null_question)
{
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    c.loop = (uv_loop_t *)&c;
    g_loop_ctx = &c;
    g_want_answer = "ok";
    char *r = ws_ask_user_cb(NULL, &c);
    g_loop_ctx = NULL;
    g_want_answer = NULL;
    ck_assert_ptr_nonnull(r);
    ck_assert(strstr(captured_ws_json, "\"question\":\"\""));
    free(r);
    free(c.ask_user_response);
    reset_capture();
}
END_TEST

START_TEST(test_ask_user_cb_times_out_without_response)
{
    /* No client reply ever arrives (g_loop_ctx = NULL, so the uv_run
     * stub leaves ask_user_done at 0); uv_now elapses past the deadline.
     * The tool must complete with a placeholder, not hang or fall
     * through to the CLI stdin fallback. */
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    c.loop = (uv_loop_t *)&c;
    c.ask_user_timeout = 1;
    char *r = ws_ask_user_cb("Time-sensitive question?", &c);
    ck_assert_ptr_nonnull(r);
    ck_assert_str_eq(r, "(user did not respond)");
    ck_assert_int_eq(c.ask_user_done, 0);
    ck_assert(strstr(captured_ws_json, "\"type\":\"ask_user\""));
    free(r);
    reset_capture();
}
END_TEST

START_TEST(test_ask_user_cb_default_timeout_never_nulls)
{
    /* ask_user_timeout == 0 must fall back to the 60s default (deadline
     * = 1000 + 60000) rather than timing out instantly; the uv_run stub
     * delivers the answer before the deadline elapses. */
    WSChatCtx c = {0};
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    c.loop = (uv_loop_t *)&c;
    g_loop_ctx = &c;
    g_want_answer = "still here";
    char *r = ws_ask_user_cb("Default timeout?", &c);
    g_loop_ctx = NULL;
    g_want_answer = NULL;
    ck_assert_ptr_nonnull(r);
    ck_assert_str_eq(r, "still here");
    free(r);
    free(c.ask_user_response);
    reset_capture();
}
END_TEST

/* ==================================================================
 * ws_chat_on_message tests
 * ================================================================== */

START_TEST(test_on_message_null_ctx)
{
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws, "{}", 2, NULL);
    reset_capture();
}
END_TEST

START_TEST(test_on_message_null_agent)
{
    WSChatCtx c = {0};
    c.agent = NULL;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws, "{}", 2, &c);
    reset_capture();
}
END_TEST

START_TEST(test_on_message_invalid_json)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws, "not json", 8, &c);
    ck_assert(strstr(captured_ws_json, "\"type\":\"error\""));
    ck_assert(strstr(captured_ws_json, "\"content\":\"invalid json\""));
    ck_assert(!strstr(captured_ws_json, "\"message\":\"invalid json\""));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_missing_type)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws, "{}", 2, &c);
    ck_assert(strstr(captured_ws_json, "missing type"));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_unsupported_type)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"unknown\"}", 19, &c);
    reset_capture();
}
END_TEST

START_TEST(test_on_message_stop)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"stop\"}", 16, &c);
    ck_assert_int_eq(c.approval_done, 1);
    ck_assert_int_eq(c.ask_user_done, 1);
    reset_capture();
}
END_TEST

START_TEST(test_on_message_approval_response)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    c.pending_request_id = "apr_1";
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"approval_response\",\"request_id\":\"apr_1\",\"approved\":true}", 68, &c);
    ck_assert_int_eq(c.approval_done, 1);
    ck_assert_int_eq(c.approval_result, 1);
    reset_capture();
}
END_TEST

START_TEST(test_on_message_approval_wrong_id)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    c.pending_request_id = "apr_1";
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"approval_response\",\"request_id\":\"apr_2\",\"approved\":true}", 68, &c);
    ck_assert_int_eq(c.approval_done, 0);
    reset_capture();
}
END_TEST

START_TEST(test_on_message_ask_user_response)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"ask_user_response\",\"answer\":\"yes\"}", 43, &c);
    ck_assert_int_eq(c.ask_user_done, 1);
    ck_assert_str_eq(c.ask_user_response, "yes");
    reset_capture();
    free(c.ask_user_response);
    c.ask_user_response = NULL;
}
END_TEST

START_TEST(test_on_message_ask_user_empty_answer)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"ask_user_response\"}", 28, &c);
    ck_assert_int_eq(c.ask_user_done, 1);
    ck_assert_str_eq(c.ask_user_response, "");
    reset_capture();
    free(c.ask_user_response);
    c.ask_user_response = NULL;
}
END_TEST

START_TEST(test_on_message_provider_config)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"ollama\",\"model\":\"llama3\"}", 40, &c);
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_provider_config_switches_provider)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    c.base_url = "http://localhost:11434";
    c.num_ctx = 4096;
    c.keep_alive_secs = 120;
    c.effort = str_dup("high");
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"lm_studio\",\"model\":\"qwen\"}", 41, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 1);
    ck_assert_str_eq(stub_agent_set_provider_name, "openai_compatible");
    ck_assert_str_eq(stub_agent_set_provider_base_url,
                     "http://localhost:1234");
    ck_assert_int_eq(stub_agent_set_provider_num_ctx, 4096);
    ck_assert_int_eq(stub_agent_set_provider_keep_alive, 120);
    ck_assert_str_eq(stub_agent_set_provider_effort, "high");
    ck_assert_str_eq(stub_agent_set_model_name, "qwen");
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    free(c.effort);
    reset_capture();
}
END_TEST

START_TEST(test_on_message_provider_config_applies_effort_override)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"openai\",\"model\":\"gpt-5-codex\",\"effort\":\"xhigh\"}", 60, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 1);
    ck_assert_str_eq(stub_agent_set_provider_effort, "xhigh");
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    ck_assert_ptr_null(strstr(captured_ws_json, "\"type\":\"error\""));
    free(c.effort);
    reset_capture();
}
END_TEST

START_TEST(test_on_message_provider_config_empty_effort_clears)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    c.effort = str_dup("high");
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"ollama\",\"effort\":\"\"}", 30, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 1);
    ck_assert_ptr_null(stub_agent_set_provider_effort);
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    free(c.effort);
    reset_capture();
}
END_TEST

START_TEST(test_on_message_provider_config_rejects_invalid_effort)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    c.effort = str_dup("high");
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"ollama\",\"effort\":\"extreme\"}", 36, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 1);
    ck_assert_str_eq(stub_agent_set_provider_effort, "high");
    ck_assert(strstr(captured_ws_json, "\"type\":\"error\""));
    ck_assert(strstr(captured_ws_json, "invalid effort value"));
    free(c.effort);
    reset_capture();
}
END_TEST

START_TEST(test_on_message_provider_config_rejects_effort_for_unsupported_provider)
{
    /* Unknown providers have no effort support; the override must be
     * rejected rather than silently forwarded. */
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    c.effort = str_dup("high");
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"anthropic\",\"effort\":\"low\"}", 38, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 1);
    ck_assert_str_eq(stub_agent_set_provider_effort, "high");
    ck_assert(strstr(captured_ws_json, "\"type\":\"error\""));
    ck_assert(strstr(captured_ws_json, "invalid effort value"));
    free(c.effort);
    reset_capture();
}
END_TEST

START_TEST(test_on_message_provider_config_opencode_zen_accepts_effort)
{
    /* Zen is the OpenAI-compatible client; it takes the same set. */
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"opencode_zen\",\"effort\":\"max\"}", 41, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 1);
    ck_assert_str_eq(stub_agent_set_provider_effort, "max");
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    ck_assert_ptr_null(strstr(captured_ws_json, "\"type\":\"error\""));
    free(c.effort);
    reset_capture();
}
END_TEST

START_TEST(test_on_message_provider_config_openai_compatible_accepts_its_set)
{
    /* openai_compatible gets low/medium/high/max/none — but not xhigh. */
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"openai_compatible\",\"effort\":\"none\"}", 49, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 1);
    ck_assert_str_eq(stub_agent_set_provider_effort, "none");
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    ck_assert_ptr_null(strstr(captured_ws_json, "\"type\":\"error\""));
    free(c.effort);
    reset_capture();

    c = (WSChatCtx){0};
    c.agent = &agent;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"openai_compatible\",\"effort\":\"xhigh\"}", 50, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 2);
    ck_assert_ptr_null(stub_agent_set_provider_effort);
    ck_assert(strstr(captured_ws_json, "\"type\":\"error\""));
    ck_assert(strstr(captured_ws_json, "invalid effort value"));
    free(c.effort);
    reset_capture();
}
END_TEST

START_TEST(test_on_message_provider_config_resolves_token_from_conf)
{
    /* The handshake resolves the per-provider token from [providers]. */
    FILE *f = fopen("/tmp/ws_test_prov.conf", "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "[providers]\nopenai = sk-ws-token\n");
    fclose(f);

    ServerContext sctx = {0};
    sctx.state = STATE_UNLOCKED;
    sctx.conf = conf_load("/tmp/ws_test_prov.conf");
    ck_assert_ptr_nonnull(sctx.conf);

    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    c.server_ctx = &sctx;
    c.base_url = "http://localhost:1234";
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"openai\",\"model\":\"gpt-4o\"}", 37, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 1);
    ck_assert_str_eq(stub_agent_set_provider_name, "openai");
    ck_assert_str_eq(stub_agent_set_provider_token, "sk-ws-token");
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    reset_capture();
    conf_free(sctx.conf);
    ck_assert_int_eq(remove("/tmp/ws_test_prov.conf"), 0);
}
END_TEST

START_TEST(test_on_message_provider_switch_uses_target_default_base_url)
{
    /* Regression: switching from ollama (startup URL localhost:11434) to
     * opencode_zen used to pass the STARTUP provider's base_url, so Zen
     * chats were POSTed to ollama and silently came back empty. The
     * switch must resolve opencode_zen's canonical default. */
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    c.base_url = "http://localhost:11434";
    c.num_ctx = 4096;
    c.keep_alive_secs = 120;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"opencode_zen\",\"model\":\"deepseek-v4-flash-free\"}",
        60, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 1);
    ck_assert_str_eq(stub_agent_set_provider_name, "opencode_zen");
    ck_assert_str_eq(stub_agent_set_provider_base_url, "https://opencode.ai/zen/v1");
    ck_assert_ptr_null(stub_agent_set_provider_token);
    ck_assert_str_eq(stub_agent_set_model_name, "deepseek-v4-flash-free");
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_provider_switch_uses_conf_base_url_override)
{
    /* [opencode_zen] base_url in conf must beat the canonical default. */
    FILE *f = fopen("/tmp/ws_test_zen.conf", "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "[opencode_zen]\nbase_url = https://zen.example.com/v1\n");
    fclose(f);

    ServerContext sctx = {0};
    sctx.state = STATE_UNLOCKED;
    sctx.conf = conf_load("/tmp/ws_test_zen.conf");
    ck_assert_ptr_nonnull(sctx.conf);

    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    c.server_ctx = &sctx;
    c.base_url = "http://localhost:11434";
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"opencode_zen\",\"model\":\"deepseek-v4-flash-free\"}",
        60, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 1);
    ck_assert_str_eq(stub_agent_set_provider_name, "opencode_zen");
    ck_assert_str_eq(stub_agent_set_provider_base_url, "https://zen.example.com/v1");
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    reset_capture();
    conf_free(sctx.conf);
    ck_assert_int_eq(remove("/tmp/ws_test_zen.conf"), 0);
}
END_TEST

START_TEST(test_on_message_provider_config_same_provider)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"ollama\",\"model\":\"llama3\"}", 40, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 1);
    ck_assert_str_eq(stub_agent_set_provider_name, "ollama");
    ck_assert_str_eq(stub_agent_set_model_name, "llama3");
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_provider_config_failure_sends_error)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    c.base_url = "http://localhost:11434";
    stub_agent_set_provider_result = -1;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"openai\",\"model\":\"gpt-4o\"}", 37, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 1);
    ck_assert_str_eq(stub_agent_set_provider_name, "openai");
    ck_assert(strstr(captured_ws_json, "provider switch failed: openai"));
    ck_assert(strstr(captured_ws_json, "\"type\":\"error\""));
    ck_assert_str_eq(stub_agent_set_model_name, "gpt-4o");
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_provider_config_model_only)
{
    /* A config message without a provider field is not a valid handshake:
     * the handler falls through to "missing type". The FE always sends
     * provider + model together. */
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"model\":\"qwen\"}", 16, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 0);
    ck_assert(strstr(captured_ws_json, "missing type"));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_message_runs_agent)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    agent.session_id = NULL;
    c.agent = &agent;
    c.ready = 1;
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_agent_run_streaming_resp = &fake_resp_basic;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"message\",\"content\":\"hello\"}", 38, &c);
    ck_assert_int_eq(stub_agent_run_streaming_count, 1);
    ck_assert(strstr(captured_ws_json, "\"type\":\"done\""));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_rejects_expired_auth_generation)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    ServerContext server_ctx = {0};
    server_ctx.state = STATE_LOCKED;
    server_ctx.auth_generation = 2;
    c.agent = &agent;
    c.server_ctx = &server_ctx;
    c.auth_generation = 1;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"message\",\"content\":\"hi\"}", 33, &c);
    ck_assert_int_eq(stub_agent_run_streaming_count, 0);
    ck_assert_ptr_nonnull(strstr(captured_ws_json, "authentication expired"));
}
END_TEST

START_TEST(test_on_message_message_null_resp)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    c.ready = 1;
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_agent_run_streaming_resp = NULL;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"message\",\"content\":\"x\"}", 33, &c);
    ck_assert(strstr(captured_ws_json, "agent returned no response"));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_message_enqueued_when_not_ready)
{
    WSChatCtx *c = calloc(1, sizeof(WSChatCtx));
    c->agent = calloc(1, sizeof(Agent));
    c->ready = 0;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"message\",\"content\":\"wait\"}", 38, c);
    ck_assert_ptr_nonnull(c->msg_queue);
    ck_assert_str_eq(c->msg_queue->data, "{\"type\":\"message\",\"content\":\"wait\"}");
    WSClient ws_dummy = {0};
    ws_chat_on_close(&ws_dummy, c);
    reset_capture();
}
END_TEST

START_TEST(test_on_message_missing_content)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"message\"}", 18, &c);
    ck_assert(strstr(captured_ws_json, "missing message content"));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_message_field_alias)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    agent.session_id = NULL;
    c.agent = &agent;
    c.ready = 1;
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_agent_run_streaming_resp = &fake_resp_basic;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"message\",\"message\":\"hi via alias\"}", 47, &c);
    ck_assert_int_eq(stub_agent_run_streaming_count, 1);
    ck_assert(strstr(captured_ws_json, "\"type\":\"done\""));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_session_id_new)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    agent.session_id = NULL;
    c.agent = &agent;
    c.sm = (SessionManager *)&c;
    c.ready = 1;
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_session_load_result = &fake_session;
    stub_agent_run_streaming_resp = &fake_resp_basic;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"message\",\"content\":\"hi\",\"session_id\":\"test-session-123\"}", 69, &c);
    ck_assert_str_eq(c.active_session_id, "test-session-123");
    /* history event is no longer sent during message processing —
     * the frontend loaded the session via REST before sending the message. */
    ck_assert(strstr(captured_ws_json, "\"type\":\"done\""));
    reset_capture();
    free(c.active_session_id);
    c.active_session_id = NULL;
    free(c.agent->session_id);
    c.agent->session_id = NULL;
    message_free_all(c.agent->messages, c.agent->messages_count);
    c.agent->messages = NULL;
    c.agent->messages_count = 0;
}
END_TEST

START_TEST(test_on_message_session_id_stale)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    c.active_session_id = "current-sess";
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"message\",\"content\":\"x\",\"session_id\":\"other-sess\"}", 60, &c);
    ck_assert(strstr(captured_ws_json, "\"content\":\"stale session_id\""));
    ck_assert(!strstr(captured_ws_json, "\"message\":\"stale session_id\""));
    reset_capture();
    c.active_session_id = NULL;
}
END_TEST

START_TEST(test_on_message_session_id_not_found)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    agent.session_id = NULL;
    c.agent = &agent;
    c.sm = (SessionManager *)&c;
    char dummy_ws = 0;
    stub_session_load_result = NULL;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"message\",\"content\":\"x\",\"session_id\":\"no-such\"}", 56, &c);
    ck_assert(strstr(captured_ws_json, "\"content\":\"session not found\""));
    ck_assert(!strstr(captured_ws_json, "\"message\":\"session not found\""));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_message_agent_gets_session_id)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    agent.session_id = "from-agent";
    c.agent = &agent;
    c.ready = 1;
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_agent_run_streaming_resp = &fake_resp_basic;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"message\",\"content\":\"hi\"}", 33, &c);
    ck_assert_str_eq(c.active_session_id, "from-agent");
    reset_capture();
    free(c.active_session_id);
    c.active_session_id = NULL;
}
END_TEST

START_TEST(test_on_message_edit_forks)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    Message *msgs = calloc(2, sizeof(Message));
    ck_assert_ptr_nonnull(msgs);
    msgs[0].role = str_dup("system");
    msgs[0].content = str_dup("You are a helpful assistant.");
    msgs[1].role = str_dup("user");
    msgs[1].content = str_dup("original message");
    agent.session_id = str_dup("edit-sess");
    agent.messages = msgs;
    agent.messages_count = 2;
    c.agent = &agent;
    c.sm = (SessionManager *)&c;
    c.ready = 0;
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_agent_run_streaming_resp = &fake_resp_basic;
    stub_fork_message_id = "m_fresh1";
    stub_fork_group_id = "fg_fresh1";
    stub_fork_content = "replacement";
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"edit\",\"index\":1,\"content\":\"replacement\"}", 54, &c);
    ck_assert_int_eq(stub_agent_run_streaming_count, 1);
    ck_assert_str_eq(c.active_session_id, "edit-sess");
    /* fork: keep=1+system_prefix(1)=2, so the 2-message context is kept
     * as-is and the minted fork message is appended as the new tail. */
    ck_assert_int_eq(agent.messages_count, 3);
    ck_assert_str_eq(agent.messages[2].content, "replacement");
    ck_assert_str_eq(agent.messages[2].role, "user");
    /* done frame carries the fresh fork identity for the pill. */
    ck_assert(strstr(captured_ws_json, "\"type\":\"done\""));
    ck_assert(strstr(captured_ws_json, "\"fork_message_id\":\"m_fresh1\""));
    ck_assert(strstr(captured_ws_json, "\"fork_group_id\":\"fg_fresh1\""));
    reset_capture();
    free(c.active_session_id);
    c.active_session_id = NULL;
    message_free_all(agent.messages, agent.messages_count);
    free(agent.session_id);
}

START_TEST(test_on_message_edit_clears_tail_then_appends_fork)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    Message *msgs = calloc(3, sizeof(Message));
    ck_assert_ptr_nonnull(msgs);
    msgs[0].role = str_dup("system");
    msgs[0].content = str_dup("system prompt");
    msgs[1].role = str_dup("user");
    msgs[1].content = str_dup("msg1");
    msgs[2].role = str_dup("assistant");
    msgs[2].content = str_dup("reply1");
    agent.session_id = str_dup("edit-sess-2");
    agent.messages = msgs;
    agent.messages_count = 3;
    c.agent = &agent;
    c.sm = (SessionManager *)&c;
    c.ready = 0;
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_agent_run_streaming_resp = NULL;
    stub_fork_content = "new msg";
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"edit\",\"index\":1,\"content\":\"new msg\"}", 49, &c);
    ck_assert_int_eq(stub_agent_run_streaming_count, 1);
    /* truncation: index=1, system_prefix=1, keep=2 — the old assistant
     * tail is dropped, then the fork message replaces it. */
    ck_assert_int_eq(agent.messages_count, 3);
    ck_assert_str_eq(agent.messages[2].content, "new msg");
    ck_assert_str_eq(agent.messages[2].role, "user");
    /* null resp → error frame sent after the fork committed */
    ck_assert(strstr(captured_ws_json, "\"type\":\"error\""));
    reset_capture();
    free(c.active_session_id);
    c.active_session_id = NULL;
    message_free_all(agent.messages, agent.messages_count);
    free(agent.session_id);
}
END_TEST

START_TEST(test_on_message_regenerate_forks_at_previous_user_turn)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    Message *msgs = calloc(3, sizeof(Message));
    ck_assert_ptr_nonnull(msgs);
    msgs[0].role = str_dup("system");
    msgs[0].content = str_dup("system prompt");
    msgs[1].role = str_dup("user");
    msgs[1].content = str_dup("ask one");
    msgs[2].role = str_dup("assistant");
    msgs[2].content = str_dup("reply one");
    agent.session_id = str_dup("regen-sess");
    agent.messages = msgs;
    agent.messages_count = 3;
    c.agent = &agent;
    c.sm = (SessionManager *)&c;
    c.ready = 0;
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_agent_run_streaming_resp = &fake_resp_basic;
    stub_fork_message_id = "m_regen";
    stub_fork_group_id = "fg_regen";
    /* regenerate keeps the original user-turn content (content=NULL path) */
    stub_fork_content = "ask one";
    /* DB index of the assistant reply is 1 (0-based, system msg excluded) —
     * the handler steps back to the user turn at DB index 0. */
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"regenerate\",\"index\":1}", 31, &c);
    ck_assert_int_eq(stub_agent_run_streaming_count, 1);
    /* fork point = user turn (DB index 0), system_prefix=1 → agent keep=1:
     * the old user/assistant tail is dropped and the fork message (the
     * re-issued user turn) is appended. */
    ck_assert_int_eq(agent.messages_count, 2);
    ck_assert_str_eq(agent.messages[1].role, "user");
    ck_assert_str_eq(agent.messages[1].content, "ask one");
    ck_assert(strstr(captured_ws_json, "\"type\":\"done\""));
    ck_assert(strstr(captured_ws_json, "\"fork_message_id\":\"m_regen\""));
    reset_capture();
    free(c.active_session_id);
    c.active_session_id = NULL;
    message_free_all(agent.messages, agent.messages_count);
    free(agent.session_id);
}

START_TEST(test_on_message_regenerate_invalid_index_errors)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    agent.session_id = str_dup("regen-sess-2");
    agent.messages = NULL;
    agent.messages_count = 0;
    c.agent = &agent;
    c.sm = (SessionManager *)&c;
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"regenerate\",\"index\":5}", 29, &c);
    ck_assert_int_eq(stub_agent_run_streaming_count, 0);
    ck_assert(strstr(captured_ws_json, "\"type\":\"error\""));
    ck_assert(strstr(captured_ws_json, "invalid index"));
    reset_capture();
    free(agent.session_id);
}

START_TEST(test_on_message_edit_rejects_negative_index)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    c.sm = (SessionManager *)&c;
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_agent_run_streaming_count = 0;
    /* L3 regression: index -1 used to reach ws_run_fork with keep = -1 and
     * clear messages[-1] (heap underflow read + free of garbage). The
     * handler must reject the frame before any fork work happens. */
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"edit\",\"index\":-1,\"content\":\"edited\"}", 42, &c);
    ck_assert_int_eq(stub_agent_run_streaming_count, 0);
    ck_assert(strstr(captured_ws_json, "\"type\":\"error\""));
    ck_assert(strstr(captured_ws_json, "invalid index"));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_edit_rejects_huge_index)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    c.agent = &agent;
    c.sm = (SessionManager *)&c;
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_agent_run_streaming_count = 0;
    /* L3 second vector: (int)1e100 is UB; a non-integral or out-of-int
     * range double must be rejected the same way as a negative index. */
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"edit\",\"index\":1e100,\"content\":\"edited\"}", 45, &c);
    ck_assert_int_eq(stub_agent_run_streaming_count, 0);
    ck_assert(strstr(captured_ws_json, "\"type\":\"error\""));
    ck_assert(strstr(captured_ws_json, "invalid index"));
    reset_capture();
}
END_TEST

START_TEST(test_on_message_branch_switch_success)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    agent.session_id = str_dup("switch-sess");
    c.agent = &agent;
    c.sm = (SessionManager *)&c;
    c.ready = 1;
    c.active_session_id = str_dup("switch-sess");
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_switch_rc = 0;
    stub_branch_info_json =
        "[{\"message_id\":\"m1\",\"count\":2,\"active\":2}]";
    stub_session_load_result = &fake_session;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"branch_switch\",\"branch_id\":\"br_x\"}", 48, &c);
    /* the loaded chain is re-sent as history and agent context swapped */
    ck_assert(strstr(captured_ws_json, "\"type\":\"history\""));
    ck_assert(strstr(captured_ws_json, "\"content\":\"Hello\""));
    ck_assert_int_eq(agent.messages_count, 1);
    ck_assert_str_eq(agent.messages[0].content, "Hello");
    ck_assert(strstr(captured_ws_json, "\"type\":\"branch_info\""));
    ck_assert(strstr(captured_ws_json, "\"active\":2"));
    reset_capture();
    free(c.active_session_id);
    message_free_all(agent.messages, agent.messages_count);
    free(agent.session_id);
}

START_TEST(test_on_message_branch_switch_not_found)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    agent.session_id = str_dup("switch-sess-2");
    c.agent = &agent;
    c.sm = (SessionManager *)&c;
    c.ready = 1;
    c.active_session_id = str_dup("switch-sess-2");
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_switch_rc = -1;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"branch_switch\",\"branch_id\":\"br_missing\"}", 51, &c);
    ck_assert(strstr(captured_ws_json, "\"type\":\"error\""));
    ck_assert(strstr(captured_ws_json, "branch not found"));
    ck_assert_int_eq(agent.messages_count, 0);
    reset_capture();
    free(c.active_session_id);
    free(agent.session_id);
}

START_TEST(test_on_message_branch_info_request)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    agent.session_id = str_dup("info-sess");
    c.agent = &agent;
    c.sm = (SessionManager *)&c;
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_branch_info_json = "[{\"message_id\":\"m9\",\"count\":1,\"active\":1}]";
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"branch_info\"}", 20, &c);
    ck_assert(strstr(captured_ws_json, "\"type\":\"branch_info\""));
    ck_assert(strstr(captured_ws_json, "\"message_id\":\"m9\""));
    reset_capture();
    free(agent.session_id);
}

/* ==================================================================
 * routes_ws_chat_init tests
 * ================================================================== */

START_TEST(test_ws_init_agent_create_fails)
{
    WSClient ws = {0};
    ServerContext ctx = {0};
    ctx.agent_cfg.provider = "ollama";
    ctx.agent_cfg.model = "llama3";
    stub_agent_create_succeeds = 0;
    routes_ws_chat_init(&ws, &ctx, NULL);
    ck_assert(ws.on_message == NULL);
    stub_agent_create_succeeds = 1;
    reset_capture();
}
END_TEST

START_TEST(test_ws_init_creates_agent_and_sends_ready)
{
    WSClient ws = {0};
    ServerContext ctx = {0};
    ctx.agent_cfg.provider = "ollama";
    ctx.agent_cfg.model = "llama3";
    routes_ws_chat_init(&ws, &ctx, NULL);
    ck_assert(ws.on_message != NULL);
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    ws_chat_on_close(&ws, ws.userdata);
    reset_capture();
}
END_TEST

START_TEST(test_ws_init_with_query_no_session_param)
{
    WSClient ws = {0};
    ServerContext ctx = {0};
    ctx.agent_cfg.provider = "ollama";
    routes_ws_chat_init(&ws, &ctx, "foo=bar");
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    ws_chat_on_close(&ws, ws.userdata);
    reset_capture();
}
END_TEST

START_TEST(test_ws_init_agent_has_session_id)
{
    WSClient ws = {0};
    ServerContext ctx = {0};
    ctx.agent_cfg.provider = "ollama";
    routes_ws_chat_init(&ws, &ctx, NULL);
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    ws_chat_on_close(&ws, ws.userdata);
    reset_capture();
}
END_TEST

START_TEST(test_ws_init_with_session_id_query)
{
    WSClient ws = {0};
    ServerContext ctx = {0};
    ctx.agent_cfg.provider = "ollama";
    ctx.agent_cfg.model = "llama3";
    ctx.sm = (SessionManager *)&ctx;
    stub_session_load_result = &fake_session;
    routes_ws_chat_init(&ws, &ctx, "session_id=test-session-123");
    ck_assert(strstr(captured_ws_json, "\"type\":\"history\""));
    ck_assert(strstr(captured_ws_json, "\"content\":\"Hello\""));
    ck_assert(strstr(captured_ws_json, "\"type\":\"session_start\""));
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
    ck_assert(strstr(captured_ws_json, "\"session_id\":\"test-session-123\""));
    ws_chat_on_close(&ws, ws.userdata);
    reset_capture();
}
END_TEST

START_TEST(test_logout_invalidation_cancels_agents_and_releases_storage)
{
    ServerContext ctx = {0};
    Agent agent = {0};
    WSChatCtx chat = {0};
    chat.agent = &agent;
    chat.sm = (SessionManager *)&ctx;
    chat.server_ctx = &ctx;
    ctx.ws_chat_contexts = &chat;

    routes_ws_invalidate_auth(&ctx);

    ck_assert_ptr_null(chat.sm);
    ck_assert_int_eq(chat.approval_done, 1);
    ck_assert_int_eq(chat.approval_result, 0);
    ck_assert_int_eq(chat.ask_user_done, 1);
    ck_assert_int_eq(stub_agent_cancel_calls, 1);
    ck_assert_int_eq(stub_agent_clear_sm_calls, 1);
    ck_assert_int_eq(stub_session_manager_free_calls, 1);
}
END_TEST

Suite *routes_ws_suite(void)
{
    Suite *s = suite_create("routes_ws");

    TCase *tc = tcase_create("ws_chat_on_chunk");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_on_chunk_forwards_content_frame);
    tcase_add_test(tc, test_on_chunk_null_chunk);
    tcase_add_test(tc, test_on_chunk_null_ctx);
    tcase_add_test(tc, test_on_chunk_null_ws);
    tcase_add_test(tc, test_on_chunk_with_session_id);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_send_done");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_send_done_emits_done_frame);
    tcase_add_test(tc, test_send_done_null_resp);
    tcase_add_test(tc, test_send_done_with_session_id);
    tcase_add_test(tc, test_send_done_with_title);
    tcase_add_test(tc, test_send_done_with_tool_calls);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_chat_emit_session_start");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_emit_session_start_no_session_id);
    tcase_add_test(tc, test_emit_session_start_with_session);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_title_update_cb");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_title_update_cb_valid);
    tcase_add_test(tc, test_title_update_cb_null_ctx);
    tcase_add_test(tc, test_title_update_cb_null_ws);
    tcase_add_test(tc, test_title_update_cb_nulls);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_tool_start_cb");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_tool_start_cb_valid);
    tcase_add_test(tc, test_tool_start_cb_nulls);
    tcase_add_test(tc, test_tool_start_cb_null_ws);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_tool_end_cb");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_tool_end_cb_valid);
    tcase_add_test(tc, test_tool_end_cb_nulls);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_chat_enqueue");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_enqueue_single);
    tcase_add_test(tc, test_enqueue_multiple);
    tcase_add_test(tc, test_enqueue_allocation_failure_leaves_queue_unchanged);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_chat_flush_queue");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_flush_queue_not_ready);
    tcase_add_test(tc, test_flush_queue_empty);
    tcase_add_test(tc, test_flush_queue_single_success);
    tcase_add_test(tc, test_flush_queue_single_null_resp);
    tcase_add_test(tc, test_flush_queue_agent_gets_session_id);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_chat_on_close");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_on_close_cleanup);
    tcase_add_test(tc, test_on_close_with_agent);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_approval_cb");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_approval_cb_null_ctx);
    tcase_add_test(tc, test_approval_cb_null_ws);
    tcase_add_test(tc, test_approval_cb_approved);
    tcase_add_test(tc, test_approval_cb_rejected);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_ask_user_cb");
    /* E11: the one TCase without a checked fixture — the timeout test
     * depends on stub_uv_now_calls being 0, which setup() resets; without
     * the fixture, serial-mode runs see a stale counter and hang. */
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_ask_user_cb_null_ctx);
    tcase_add_test(tc, test_ask_user_cb_null_ws);
    tcase_add_test(tc, test_ask_user_cb_with_answer);
    tcase_add_test(tc, test_ask_user_cb_null_question);
    tcase_add_test(tc, test_ask_user_cb_times_out_without_response);
    tcase_add_test(tc, test_ask_user_cb_default_timeout_never_nulls);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_chat_on_message");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_on_message_null_ctx);
    tcase_add_test(tc, test_on_message_null_agent);
    tcase_add_test(tc, test_on_message_invalid_json);
    tcase_add_test(tc, test_on_message_missing_type);
    tcase_add_test(tc, test_on_message_unsupported_type);
    tcase_add_test(tc, test_on_message_stop);
    tcase_add_test(tc, test_on_message_approval_response);
    tcase_add_test(tc, test_on_message_approval_wrong_id);
    tcase_add_test(tc, test_on_message_ask_user_response);
    tcase_add_test(tc, test_on_message_ask_user_empty_answer);
    tcase_add_test(tc, test_on_message_provider_config);
    tcase_add_test(tc, test_on_message_provider_config_switches_provider);
    tcase_add_test(tc, test_on_message_provider_config_applies_effort_override);
    tcase_add_test(tc, test_on_message_provider_config_empty_effort_clears);
    tcase_add_test(tc, test_on_message_provider_config_rejects_invalid_effort);
    tcase_add_test(tc, test_on_message_provider_config_rejects_effort_for_unsupported_provider);
    tcase_add_test(tc, test_on_message_provider_config_openai_compatible_accepts_its_set);
    tcase_add_test(tc, test_on_message_provider_config_opencode_zen_accepts_effort);
    tcase_add_test(tc, test_on_message_provider_config_resolves_token_from_conf);
    tcase_add_test(tc, test_on_message_provider_switch_uses_target_default_base_url);
    tcase_add_test(tc, test_on_message_provider_switch_uses_conf_base_url_override);
    tcase_add_test(tc, test_on_message_provider_config_same_provider);
    tcase_add_test(tc, test_on_message_provider_config_failure_sends_error);
    tcase_add_test(tc, test_on_message_provider_config_model_only);
    tcase_add_test(tc, test_on_message_message_runs_agent);
    tcase_add_test(tc, test_on_message_rejects_expired_auth_generation);
    tcase_add_test(tc, test_on_message_message_null_resp);
    tcase_add_test(tc, test_on_message_message_enqueued_when_not_ready);
    tcase_add_test(tc, test_on_message_missing_content);
    tcase_add_test(tc, test_on_message_message_field_alias);
    tcase_add_test(tc, test_on_message_session_id_new);
    tcase_add_test(tc, test_on_message_session_id_stale);
    tcase_add_test(tc, test_on_message_session_id_not_found);
    tcase_add_test(tc, test_on_message_message_agent_gets_session_id);
    tcase_add_test(tc, test_on_message_edit_forks);
    tcase_add_test(tc, test_on_message_edit_clears_tail_then_appends_fork);
    tcase_add_test(tc, test_on_message_edit_disconnect_during_run_is_safe);
    tcase_add_test(tc, test_on_message_edit_rejects_negative_index);
    tcase_add_test(tc, test_on_message_edit_rejects_huge_index);
    tcase_add_test(tc, test_on_message_regenerate_forks_at_previous_user_turn);
    tcase_add_test(tc, test_on_message_regenerate_invalid_index_errors);
    tcase_add_test(tc, test_on_message_branch_switch_success);
    tcase_add_test(tc, test_on_message_branch_switch_not_found);
    tcase_add_test(tc, test_on_message_branch_info_request);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    tc = tcase_create("routes_ws_chat_init");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_ws_init_agent_create_fails);
    tcase_add_test(tc, test_ws_init_creates_agent_and_sends_ready);
    tcase_add_test(tc, test_ws_init_with_query_no_session_param);
    tcase_add_test(tc, test_ws_init_agent_has_session_id);
    tcase_add_test(tc, test_ws_init_with_session_id_query);
    tcase_add_test(tc,
                   test_logout_invalidation_cancels_agents_and_releases_storage);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    Suite *s = routes_ws_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed;
}
