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
#include "../src/utils/string_utils.h"

/* WSChatCtx mirror — matches routes_ws.c */
typedef struct QueuedMsg { char *data; struct QueuedMsg *next; } QueuedMsg;
typedef struct {
    Agent *agent; SessionManager *sm; SafetyConfig *safety; WSClient *ws;
    uv_loop_t *loop; char *pending_request_id; int approval_done;
    int approval_result; int ready; QueuedMsg *msg_queue;
    QueuedMsg *msg_queue_tail; char *active_session_id;
    int session_start_emitted; int ask_user_done; char *ask_user_response;
    int ask_user_timeout;
    const char *base_url; const char *api_token;
    int num_ctx; int keep_alive_secs;
    int active_runs; int closing;
    ServerContext *server_ctx; unsigned long auth_generation;
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
static const char *stub_agent_set_model_name = NULL;

static LLMResponse fake_resp_basic = {0};
static LLMResponse fake_resp_with_tools = {0};
static ToolCall fake_tools[2] = {0};
static Session fake_session = {0};
static Message fake_session_msgs[1] = {0};

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
{ (void)ws; (void)data; (void)len; return 0; }

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

LLMResponse *agent_run_streaming(Agent *agent, const char *input,
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
void agent_cancel(Agent *a) { (void)a; }
void agent_destroy(Agent *a) {
    if (!a) return;
    free(a->session_id);
    free(a->messages);
    free(a);
}

Agent *agent_create(const AgentConfig *cfg)
{
    (void)cfg;
    if (!stub_agent_create_succeeds) return NULL;
    return calloc(1, sizeof(Agent));
}

void agent_set_session_manager(Agent *a, SessionManager *sm) { (void)a; (void)sm; }
void agent_set_approval_callback(Agent *a, int (*cb)(const char *, const char *, void *), void *u)
{ (void)a; (void)cb; (void)u; }
void agent_set_title_callback(Agent *a, title_callback cb, void *u) { (void)a; (void)cb; (void)u; }
void agent_set_tool_start_callback(Agent *a, tool_start_callback cb, void *u) { (void)a; (void)cb; (void)u; }
void agent_set_tool_end_callback(Agent *a, tool_end_callback cb, void *u) { (void)a; (void)cb; (void)u; }
void agent_set_ask_user_callback(Agent *a, ask_user_callback cb, void *u)
{ (void)a; (void)cb; (void)u; }
void agent_set_safety(Agent *a, SafetyConfig *s) { (void)a; (void)s; }
void agent_set_metrics(Agent *a, Metrics *m) { (void)a; (void)m; }

int agent_set_provider(Agent *a, const char *provider, const char *base_url,
                       const char *api_token, int num_ctx, int keep_alive_secs)
{
    (void)a;
    stub_agent_set_provider_calls++;
    free((char *)stub_agent_set_provider_name);
    free((char *)stub_agent_set_provider_base_url);
    free((char *)stub_agent_set_provider_token);
    stub_agent_set_provider_name = str_dup(provider);
    stub_agent_set_provider_base_url = str_dup(base_url);
    stub_agent_set_provider_token = str_dup(api_token);
    stub_agent_set_provider_num_ctx = num_ctx;
    stub_agent_set_provider_keep_alive = keep_alive_secs;
    return stub_agent_set_provider_result;
}

void agent_set_model(Agent *a, const char *m)
{
    (void)a;
    free((char *)stub_agent_set_model_name);
    stub_agent_set_model_name = str_dup(m);
}
void agent_set_callback_manager(Agent *a, CallbackManager *m) { (void)a; (void)m; }

/* factory.c (linked for provider_default_base_url) references these
 * provider constructors, but nothing in routes_ws.c calls get_provider
 * — the agent layer is stubbed above. Stub them so the real factory
 * mapping links without dragging in live LLM clients. */
LLMProvider *ollama_provider_create(const char *b, int n, int k)
{ (void)b; (void)n; (void)k; return NULL; }
LLMProvider *openai_compatible_provider_create(const char *b, const char *t)
{ (void)b; (void)t; return NULL; }
LLMProvider *openai_provider_create(const char *b, const char *t)
{ (void)b; (void)t; return NULL; }
LLMProvider *opencode_zen_provider_create(const char *b, const char *t)
{ (void)b; (void)t; return NULL; }

Session *session_manager_load_session(SessionManager *sm, const char *id)
{ (void)sm; (void)id; return stub_session_load_result; }

int session_manager_truncate_history(SessionManager *sm, const char *sid, int idx)
{ (void)sm; (void)sid; (void)idx; return 0; }

int message_copy(Message *dst, const Message *src) { (void)dst; (void)src; return 0; }
void message_free_all(Message *msgs, int count) { (void)msgs; (void)count; }
void message_clear(Message *msg) { (void)msg; }

void session_free(Session *s)
{
    if (s && s != &fake_session) free(s);
}

void registry_set_ask_user_callback(char *(*cb)(const char *, void *), void *u)
{ (void)cb; (void)u; }

void log_error(const char *fmt, ...) { (void)fmt; }
void log_init(void) {}
void log_cleanup(void) {}
void log_set_level(int l) { (void)l; }
void log_msg(int l, const char *f, int line, const char *fmt, ...)
{ (void)l; (void)f; (void)line; (void)fmt; }

/* ==================================================================
 * ws_chat_on_chunk tests
 * ================================================================== */

START_TEST(test_on_chunk_basic)
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

START_TEST(test_send_done_basic)
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
    ck_assert(strstr(captured_ws_json, "invalid json"));
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
    char dummy_ws = 0;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"provider\":\"lm_studio\",\"model\":\"qwen\"}", 41, &c);
    ck_assert_int_eq(stub_agent_set_provider_calls, 1);
    ck_assert_str_eq(stub_agent_set_provider_name, "openai");
    /* Switches resolve the target provider's own URL, never the startup
     * provider's — lm_studio maps to openai, whose canonical default is
     * api.openai.com even though the session started on localhost. */
    ck_assert_str_eq(stub_agent_set_provider_base_url, "https://api.openai.com");
    ck_assert_int_eq(stub_agent_set_provider_num_ctx, 4096);
    ck_assert_int_eq(stub_agent_set_provider_keep_alive, 120);
    ck_assert_str_eq(stub_agent_set_model_name, "qwen");
    ck_assert(strstr(captured_ws_json, "\"type\":\"ready\""));
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
    remove("/tmp/ws_test_prov.conf");
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
    remove("/tmp/ws_test_zen.conf");
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

START_TEST(test_on_message_message_simple)
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
    free(c.agent->messages);
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
    ck_assert(strstr(captured_ws_json, "stale session_id"));
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
    ck_assert(strstr(captured_ws_json, "session not found"));
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

START_TEST(test_on_message_edit_truncate)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    Message msgs[2] = {0};
    msgs[0].role = "system";
    msgs[0].content = "You are a helpful assistant.";
    msgs[1].role = "user";
    msgs[1].content = "original message";
    agent.session_id = "edit-sess";
    agent.messages = msgs;
    agent.messages_count = 2;
    c.agent = &agent;
    c.sm = (SessionManager *)&c;
    c.ready = 0;
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_agent_run_streaming_resp = &fake_resp_basic;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"edit\",\"index\":1,\"content\":\"replacement\"}", 54, &c);
    ck_assert_int_eq(stub_agent_run_streaming_count, 1);
    ck_assert_str_eq(c.active_session_id, "edit-sess");
    ck_assert(strstr(captured_ws_json, "\"type\":\"done\""));
    /* truncation: index=1, system_prefix=1, keep=2, so no clearing needed */
    reset_capture();
    free(c.active_session_id);
    c.active_session_id = NULL;
}
END_TEST

START_TEST(test_on_message_edit_truncate_clears)
{
    WSChatCtx c = {0};
    Agent agent = {0};
    Message msgs[3] = {0};
    msgs[0].role = "system";
    msgs[0].content = "system prompt";
    msgs[1].role = "user";
    msgs[1].content = "msg1";
    msgs[2].role = "assistant";
    msgs[2].content = "reply1";
    agent.session_id = "edit-sess-2";
    agent.messages = msgs;
    agent.messages_count = 3;
    c.agent = &agent;
    c.sm = (SessionManager *)&c;
    c.ready = 0;
    char dummy_ws = 0;
    c.ws = (WSClient *)&dummy_ws;
    stub_agent_run_streaming_resp = NULL;
    ws_chat_on_message((WSClient *)&dummy_ws,
        "{\"type\":\"edit\",\"index\":1,\"content\":\"new msg\"}", 49, &c);
    ck_assert_int_eq(stub_agent_run_streaming_count, 1);
    /* truncation: index=1, system_prefix=1, keep=2, clears msg[2] */
    ck_assert_int_eq(agent.messages_count, 2);
    /* null resp → error frame sent */
    ck_assert(strstr(captured_ws_json, "\"type\":\"error\""));
    reset_capture();
    free(c.active_session_id);
    c.active_session_id = NULL;
}
END_TEST

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

START_TEST(test_ws_init_basic)
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

Suite *routes_ws_suite(void)
{
    Suite *s = suite_create("routes_ws");

    TCase *tc = tcase_create("ws_chat_on_chunk");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_on_chunk_basic);
    tcase_add_test(tc, test_on_chunk_null_chunk);
    tcase_add_test(tc, test_on_chunk_null_ctx);
    tcase_add_test(tc, test_on_chunk_null_ws);
    tcase_add_test(tc, test_on_chunk_with_session_id);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_send_done");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_send_done_basic);
    tcase_add_test(tc, test_send_done_null_resp);
    tcase_add_test(tc, test_send_done_with_session_id);
    tcase_add_test(tc, test_send_done_with_title);
    tcase_add_test(tc, test_send_done_with_tool_calls);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_chat_emit_session_start");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_emit_session_start_no_session_id);
    tcase_add_test(tc, test_emit_session_start_with_session);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_title_update_cb");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_title_update_cb_valid);
    tcase_add_test(tc, test_title_update_cb_null_ctx);
    tcase_add_test(tc, test_title_update_cb_null_ws);
    tcase_add_test(tc, test_title_update_cb_nulls);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_tool_start_cb");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_tool_start_cb_valid);
    tcase_add_test(tc, test_tool_start_cb_nulls);
    tcase_add_test(tc, test_tool_start_cb_null_ws);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_tool_end_cb");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_tool_end_cb_valid);
    tcase_add_test(tc, test_tool_end_cb_nulls);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_chat_enqueue");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_enqueue_single);
    tcase_add_test(tc, test_enqueue_multiple);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_chat_flush_queue");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_flush_queue_not_ready);
    tcase_add_test(tc, test_flush_queue_empty);
    tcase_add_test(tc, test_flush_queue_single_success);
    tcase_add_test(tc, test_flush_queue_single_null_resp);
    tcase_add_test(tc, test_flush_queue_agent_gets_session_id);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_chat_on_close");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_on_close_cleanup);
    tcase_add_test(tc, test_on_close_with_agent);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_approval_cb");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_approval_cb_null_ctx);
    tcase_add_test(tc, test_approval_cb_null_ws);
    tcase_add_test(tc, test_approval_cb_approved);
    tcase_add_test(tc, test_approval_cb_rejected);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_ask_user_cb");
    tcase_add_test(tc, test_ask_user_cb_null_ctx);
    tcase_add_test(tc, test_ask_user_cb_null_ws);
    tcase_add_test(tc, test_ask_user_cb_with_answer);
    tcase_add_test(tc, test_ask_user_cb_null_question);
    tcase_add_test(tc, test_ask_user_cb_times_out_without_response);
    tcase_add_test(tc, test_ask_user_cb_default_timeout_never_nulls);
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
    tcase_add_test(tc, test_on_message_provider_config_resolves_token_from_conf);
    tcase_add_test(tc, test_on_message_provider_switch_uses_target_default_base_url);
    tcase_add_test(tc, test_on_message_provider_switch_uses_conf_base_url_override);
    tcase_add_test(tc, test_on_message_provider_config_same_provider);
    tcase_add_test(tc, test_on_message_provider_config_failure_sends_error);
    tcase_add_test(tc, test_on_message_provider_config_model_only);
    tcase_add_test(tc, test_on_message_message_simple);
    tcase_add_test(tc, test_on_message_rejects_expired_auth_generation);
    tcase_add_test(tc, test_on_message_message_null_resp);
    tcase_add_test(tc, test_on_message_message_enqueued_when_not_ready);
    tcase_add_test(tc, test_on_message_missing_content);
    tcase_add_test(tc, test_on_message_message_field_alias);
    tcase_add_test(tc, test_on_message_session_id_new);
    tcase_add_test(tc, test_on_message_session_id_stale);
    tcase_add_test(tc, test_on_message_session_id_not_found);
    tcase_add_test(tc, test_on_message_message_agent_gets_session_id);
    tcase_add_test(tc, test_on_message_edit_truncate);
    tcase_add_test(tc, test_on_message_edit_truncate_clears);
    tcase_add_test(tc, test_on_message_edit_disconnect_during_run_is_safe);
    suite_add_tcase(s, tc);

    tc = tcase_create("routes_ws_chat_init");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_ws_init_agent_create_fails);
    tcase_add_test(tc, test_ws_init_basic);
    tcase_add_test(tc, test_ws_init_with_query_no_session_param);
    tcase_add_test(tc, test_ws_init_agent_has_session_id);
    tcase_add_test(tc, test_ws_init_with_session_id_query);
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
