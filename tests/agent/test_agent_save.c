#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sqlite3.h>
#include "agent/agent.h"
#include "agent/message.h"
#include "agent/agent_internal.h"
#include "session/session_manager.h"
#include "session/memory.h"
#include "tools/tool.h"
#include "tools/registry.h"
#include "utils/string_utils.h"
#include "utils/callbacks.h"
#include "utils/circuit_breaker.h"

/* test_agent_save - unit tests for agent save. Depends on: check, the module under test. */
/* agent_save_session is non-static for testability (see AGENTS.md §6); declare
 * it ourselves since it's not in agent.h. */
void agent_save_session(Agent *agent);

/* AGENT_TEST seams in agent.c (see the guard in that file). */
void agent_test_set_realloc_fail(int nth);
int agent_test_execute_tool_calls(Agent *agent, ToolCall *calls, int count);

/* Controllable registry: the default stub returns NULL (tool not found);
 * tests can install a stub tool to exercise the real execute path. */
static Tool *stub_registry_tool = NULL;
Tool *registry_get(const char *name) { (void)name; return stub_registry_tool; }
char *registry_schemas_json(void) { return str_dup("[]"); }
int registry_get_delegate_config(const char **a, const char **b, const char **c,
                                  const char **d, int *e, int *f, double *g,
                                  int *h, int *i)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i;
    return 0;
}
void registry_set_delegate_config(const char *a, const char *b, const char *c,
                                   const char *d, int e, int f, double g, int h, int i)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i;
}
void registry_set_session_manager(SessionManager *sm) { (void)sm; }
void registry_set_ask_user_callback(char *(*cb)(const char *, void *), void *u)
 {
    (void)cb;
    (void)u;
}

typedef struct {
    LLMProvider provider;
} MockProvider;

static LLMResponse *mock_chat(LLMProvider *self, Message *messages, int count,
                                const char *model, double temperature, int timeout,
                                const char *tools_json)
{
    (void)self; (void)messages; (void)count; (void)model; (void)temperature;
    (void)timeout; (void)tools_json;
    return NULL;
}
static LLMResponse *mock_chat_streaming(LLMProvider *self, Message *messages, int count,
                                         const char *model, double temp, int timeout,
                                         void (*on_chunk)(const char *, void *),
                                         void *userdata, const char *tools_json)
{
    (void)self; (void)messages; (void)count; (void)model; (void)temp; (void)timeout;
    (void)on_chunk; (void)userdata; (void)tools_json;
    return NULL;
}
static void mock_destroy(LLMProvider *self) { free(self); }

LLMProvider *get_provider(const char *name, const char *model, const char *base_url,
                           const char *api_token, int num_ctx, int keep_alive_secs,
                           const char *effort)
{
    (void)name; (void)model; (void)base_url; (void)api_token;
    (void)num_ctx; (void)keep_alive_secs; (void)effort;
    MockProvider *mock = calloc(1, sizeof(MockProvider));
    if (!mock) return NULL;
    mock->provider.chat = mock_chat;
    mock->provider.chat_streaming = mock_chat_streaming;
    mock->provider.destroy = mock_destroy;
    return (LLMProvider *)mock;
}

CircuitBreaker *cb_create(int threshold, int reset_ms)
{
    (void)threshold; (void)reset_ms;
    return calloc(1, sizeof(CircuitBreaker));
}
void cb_destroy(CircuitBreaker *cb) { free(cb); }
int cb_is_available(CircuitBreaker *cb) { (void)cb; return 1; }
void cb_record_success(CircuitBreaker *cb) { (void)cb; }
void cb_record_failure(CircuitBreaker *cb) { (void)cb; }

CallbackManager *cb_manager_create(void) { return NULL; }
void cb_manager_run_start(CallbackManager *m, const char *id, const char *p)
 {
    (void)m;
    (void)id;
    (void)p;
}
void cb_manager_run_end(CallbackManager *m, const char *id, const char *p)
 {
    (void)m;
    (void)id;
    (void)p;
}
void cb_manager_run_error(CallbackManager *m, const char *id, const char *e)
 {
    (void)m;
    (void)id;
    (void)e;
}
void cb_manager_llm_start(CallbackManager *m, const char *id, int n)
 {
    (void)m;
    (void)id;
    (void)n;
}
void cb_manager_llm_end(CallbackManager *m, const char *id, const char *c)
 {
    (void)m;
    (void)id;
    (void)c;
}
void cb_manager_tool_start(CallbackManager *m, const char *id, const char *n, const char *a)
 {
    (void)m;
    (void)id;
    (void)n;
    (void)a;
}
void cb_manager_tool_end(CallbackManager *m, const char *id, const char *n, const char *c)
 {
    (void)m;
    (void)id;
    (void)n;
    (void)c;
}
void cb_manager_tool_error(CallbackManager *m, const char *id, const char *n, const char *e)
 {
    (void)m;
    (void)id;
    (void)n;
    (void)e;
}

Message *apply_context_window(Message *msgs, int *count, int max_msgs, int max_chars)
{
    (void)count; (void)max_msgs; (void)max_chars;
    return msgs; /* identity — keep all messages */
}

char *split_thinking_content_dup(const char *raw) { return str_dup(raw); }

int metrics_counter_inc(Metrics *m, const char *n, const char *h) { (void)m; (void)n; (void)h; return 0; }
int metrics_histogram_observe(Metrics *m, const char *n, const char *h, double v,
                                const double *b, int bn) { (void)m; (void)n; (void)h; (void)v; (void)b; (void)bn; return 0; }

/* safety.c is NOT in the build target for this test; provide the one symbol
 * agent.c references. Always returns 0 — agent_save_session never reaches the
 * approval path, so the actual value is irrelevant for this test. */
int safety_needs_approval(const SafetyConfig *cfg, const char *tool_name)
 {
    (void)cfg;
    (void)tool_name;
    return 0;
}

/* Regression test for C6: agent_save_session used to skip the save entirely
 * when `s->messages_count == 0`. After a /clear or any path that reduces
 * agent->messages_count to 0, the save was skipped and the DB row retained
 * its stale messages. The fix removes the guard so the empty state is
 * always persisted, matching session_manager_truncate_history.
 *
 * Test: build a fake Agent with messages_count>0, save (DB has the message),
 * then reset agent->messages_count to 0 and call agent_save_session. Reload
 * the session — on old code messages_count would still be 1 (DB unchanged),
 * on new code messages_count is 0 (DB overwritten with the empty state). */
START_TEST(test_agent_save_session_persists_empty_state)
{
    char tmpdir[] = "/tmp/test_agent_save_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));

    SessionManager *sm = session_manager_create(tmpdir, "test-password");
    ck_assert_ptr_nonnull(sm);

    /* Construct a minimal Agent by hand — agent_create would need a real
     * provider; agent_save_session only reads sm, session_id, messages,
     * messages_count, so we can fill just those fields. */
    Agent agent;
    memset(&agent, 0, sizeof(agent));
    agent.sm = sm;

    /* Pretend the agent has one user message and a session id assigned. */
    agent.messages = calloc(1, sizeof(Message));
    ck_assert_ptr_nonnull(agent.messages);
    agent.messages_count = 1;
    agent.messages[0].role = str_dup("user");
    agent.messages[0].content = str_dup("hello");
    agent.messages[0].timestamp = 1000.0;

    /* First save: agent_save_session with messages_count > 0 (was always
     * saved under old code too). This both mints session_id and persists. */
    agent_save_session(&agent);
    ck_assert_ptr_nonnull(agent.session_id);

    /* Confirm the message landed in the DB. */
    Session *s = session_manager_load_session_alloc(sm, agent.session_id);
    ck_assert_ptr_nonnull(s);
    ck_assert_int_eq(s->messages_count, 1);
    ck_assert_str_eq(s->messages[0].content, "hello");
    session_free(s);

    /* Simulate /clear: agent->messages_count = 0 (free the array, set count
     * to 0). Under old code, agent_save_session's `if (messages_count > 0)`
     * guard skipped the save, leaving the row with one message. Under new
     * code, the save is always made and the DB's messages_encrypted is
     * overwritten with an empty array. */
    free(agent.messages[0].role);
    free(agent.messages[0].content);
    free(agent.messages);
    agent.messages = NULL;
    agent.messages_count = 0;

    agent_save_session(&agent);

    /* Reload and assert the empty state is now reflected in the DB. */
    s = session_manager_load_session_alloc(sm, agent.session_id);
    ck_assert_ptr_nonnull(s);
    ck_assert_int_eq(s->messages_count, 0);
    session_free(s);

    free(agent.session_id);
    session_manager_free(sm);

    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int syst = system(rm);
    (void)syst;
}
END_TEST

/* ---- execute_tool_calls (B3) ---- */

static ToolResult *stub_tool_execute_ok(Tool *self, const char *args_json)
{
    (void)self;
    (void)args_json;
    return tool_result_create("tool output text");
}

static void stub_tool_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self);
}

static Tool *make_stub_tool(void)
{
    Tool *t = calloc(1, sizeof(Tool));
    ck_assert_ptr_nonnull(t);
    t->name = str_dup("fake");
    t->description = str_dup("stub");
    t->parameters_schema = str_dup("{}");
    t->execute = stub_tool_execute_ok;
    t->destroy = stub_tool_destroy;
    return t;
}

static void free_calls(ToolCall *calls, int count)
{
    for (int i = 0; i < count; i++)
    {
        free(calls[i].id);
        free(calls[i].name);
        free(calls[i].arguments);
        free(calls[i].result_content);
        free(calls[i].result_error);
    }
    free(calls);
}

/* Happy path: a resolved tool's result lands in the agent's message list. */
START_TEST(test_execute_tool_calls_appends_tool_result)
{
    Agent agent;
    memset(&agent, 0, sizeof(agent));
    Tool *tool = make_stub_tool();
    stub_registry_tool = tool;

    ToolCall *calls = calloc(1, sizeof(ToolCall));
    ck_assert_ptr_nonnull(calls);
    calls[0].id = str_dup("call_1");
    calls[0].name = str_dup("fake");
    calls[0].arguments = str_dup("{}");
    ck_assert_int_eq(agent_test_execute_tool_calls(&agent, calls, 1), 0);
    ck_assert_int_eq(agent.messages_count, 1);
    ck_assert_str_eq(agent.messages[0].content, "tool output text");
    ck_assert_str_eq(agent.messages[0].tool_name, "fake");
    ck_assert_str_eq(agent.messages[0].tool_call_id, "call_1");

    message_free_all(agent.messages, agent.messages_count);
    free_calls(calls, 1);
    stub_registry_tool = NULL;
    stub_tool_destroy(tool);
}
END_TEST

/* B3 regression: a failed message-array growth must free the built message
 * (no leak) and leave the conversation count untouched — the old code
 * leaked the message's str_dup'd fields and ignored the failure. */
START_TEST(test_execute_tool_calls_append_oom_keeps_count)
{
    Agent agent;
    memset(&agent, 0, sizeof(agent));
    Tool *tool = make_stub_tool();
    stub_registry_tool = tool;

    ToolCall *calls = calloc(1, sizeof(ToolCall));
    ck_assert_ptr_nonnull(calls);
    calls[0].id = str_dup("call_1");
    calls[0].name = str_dup("fake");
    calls[0].arguments = str_dup("{}");

    /* First realloc in the executor is the append's growth */
    agent_test_set_realloc_fail(1);
    ck_assert_int_eq(agent_test_execute_tool_calls(&agent, calls, 1), 0);
    agent_test_set_realloc_fail(-1);
    ck_assert_int_eq(agent.messages_count, 0);
    ck_assert_ptr_null(agent.messages);

    free_calls(calls, 1);
    stub_registry_tool = NULL;
    stub_tool_destroy(tool);
}
END_TEST

/* B3 regression: message_create failing (OOM on the tool-result message)
 * used to be dereferenced unconditionally — a NULL crash. The executor
 * must skip the tool's contribution cleanly instead. */
START_TEST(test_execute_tool_calls_message_oom_is_handled)
{
    Agent agent;
    memset(&agent, 0, sizeof(agent));
    Tool *tool = make_stub_tool();
    stub_registry_tool = tool;

    ToolCall *calls = calloc(1, sizeof(ToolCall));
    ck_assert_ptr_nonnull(calls);
    calls[0].id = str_dup("call_1");
    calls[0].name = str_dup("fake");
    calls[0].arguments = str_dup("{}");

    /* message_create: calloc(1) + role str_dup(2) + content str_dup(3) */
    message_test_set_alloc_fail(1);
    ck_assert_int_eq(agent_test_execute_tool_calls(&agent, calls, 1), 0);
    message_test_set_alloc_fail(-1);
    ck_assert_int_eq(agent.messages_count, 0);

    free_calls(calls, 1);
    stub_registry_tool = NULL;
    stub_tool_destroy(tool);
}
END_TEST

/* 2026-08-12 crash regression: execute_tool_calls read agent_append_message's
 * return value (the append INDEX) as a success flag, so any tool message
 * landing at a non-zero index hit the "OOM" branch and message_free'd the
 * struct while the array still referenced its fields — teardown then
 * double-freed (Abort trap in message_clear, "pointer being freed was not
 * allocated"). The executor must treat every index >= 0 as success; this
 * test fails on the old code with a use-after-free on the field reads. */
START_TEST(test_execute_tool_calls_nonzero_index_is_not_failure)
{
    Agent agent;
    memset(&agent, 0, sizeof(agent));
    Tool *tool = make_stub_tool();
    stub_registry_tool = tool;

    /* Prime the array so the tool message lands at idx=1, not idx=0. */
    Message *first = message_create("user", "prime the array");
    ck_assert_ptr_nonnull(first);
    ck_assert_int_ge(agent_append_message(&agent, first), 0);
    free(first); /* struct only: fields moved into the array */

    ToolCall *calls = calloc(1, sizeof(ToolCall));
    ck_assert_ptr_nonnull(calls);
    calls[0].id = str_dup("call_1");
    calls[0].name = str_dup("fake");
    calls[0].arguments = str_dup("{}");

    ck_assert_int_eq(agent_test_execute_tool_calls(&agent, calls, 1), 0);
    ck_assert_int_eq(agent.messages_count, 2);
    /* The tool message must still own its fields — the old code freed them
     * via the misread "failure" branch. */
    ck_assert_str_eq(agent.messages[1].role, "tool");
    ck_assert_str_eq(agent.messages[1].content, "tool output text");
    ck_assert_str_eq(agent.messages[1].tool_name, "fake");
    ck_assert_str_eq(agent.messages[1].tool_call_id, "call_1");

    message_free_all(agent.messages, agent.messages_count);
    free_calls(calls, 1);
    stub_registry_tool = NULL;
    stub_tool_destroy(tool);
}
END_TEST

/* 2026-08-14 leak regression: agent_run_streaming_new appended the user
 * message and never freed the caller-owned struct shell (agent_append_message
 * copies the struct; the doc contract says the caller frees it, as
 * execute_tool_calls does). Every run leaked sizeof(Message) bytes, which
 * LeakSanitizer flagged at TUI quit (exit code 1). The old code fails this
 * test under LSAN: the user message shell is unreachable after the run. */
START_TEST(test_run_streaming_frees_user_message_shell)
{
    Agent agent;
    memset(&agent, 0, sizeof(agent));
    agent.provider = get_provider("test", "m", "http://localhost", NULL,
                                  0, 0, NULL);
    ck_assert_ptr_nonnull(agent.provider);
    agent.max_iterations = 1;
    agent.max_context_messages = 100;
    agent.max_context_chars = 100000;

    /* The mock provider always fails, so the run appends the user message
     * (plus the injected system prompt) and ends without an assistant
     * message — exactly the path that leaked. */
    LLMResponse *resp = agent_run_streaming_new(&agent, "hello", NULL, NULL);
    ck_assert_ptr_null(resp);
    ck_assert_int_eq(agent.messages_count, 2);
    ck_assert_str_eq(agent.messages[0].role, "system");
    ck_assert_str_eq(agent.messages[1].role, "user");
    ck_assert_str_eq(agent.messages[1].content, "hello");

    message_free_all(agent.messages, agent.messages_count);
    agent.provider->destroy(agent.provider);
}
END_TEST

Suite *agent_suite(void)
{
    Suite *s = suite_create("Agent");
    TCase *tc = tcase_create("Core");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_agent_save_session_persists_empty_state);
    tcase_add_test(tc, test_execute_tool_calls_appends_tool_result);
    tcase_add_test(tc, test_execute_tool_calls_append_oom_keeps_count);
    tcase_add_test(tc, test_execute_tool_calls_message_oom_is_handled);
    tcase_add_test(tc, test_execute_tool_calls_nonzero_index_is_not_failure);
    tcase_add_test(tc, test_run_streaming_frees_user_message_shell);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = agent_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
