#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sqlite3.h>
#include "agent/agent.h"
#include "agent/message.h"
#include "session/session_manager.h"
#include "session/memory.h"
#include "tools/tool.h"
#include "tools/registry.h"
#include "utils/string_utils.h"
#include "utils/callbacks.h"
#include "utils/circuit_breaker.h"

/* agent_save_session is non-static for testability (see AGENTS.md §6); declare
 * it ourselves since it's not in agent.h. */
void agent_save_session(Agent *agent);

/* Mock registry + provider symbols so agent.c links without dragging in the
 * whole tool chain. agent_save_session itself never touches these, but
 * agent.c as a translation unit references them. */
Tool *registry_get(const char *name) { (void)name; return NULL; }
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
{ (void)cb; (void)u; }

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
{ (void)m; (void)id; (void)p; }
void cb_manager_run_end(CallbackManager *m, const char *id, const char *p)
{ (void)m; (void)id; (void)p; }
void cb_manager_run_error(CallbackManager *m, const char *id, const char *e)
{ (void)m; (void)id; (void)e; }
void cb_manager_llm_start(CallbackManager *m, const char *id, int n)
{ (void)m; (void)id; (void)n; }
void cb_manager_llm_end(CallbackManager *m, const char *id, const char *c)
{ (void)m; (void)id; (void)c; }
void cb_manager_tool_start(CallbackManager *m, const char *id, const char *n, const char *a)
{ (void)m; (void)id; (void)n; (void)a; }
void cb_manager_tool_end(CallbackManager *m, const char *id, const char *n, const char *c)
{ (void)m; (void)id; (void)n; (void)c; }
void cb_manager_tool_error(CallbackManager *m, const char *id, const char *n, const char *e)
{ (void)m; (void)id; (void)n; (void)e; }

Message *apply_context_window(Message *msgs, int *count, int max_msgs, int max_chars)
{
    (void)count; (void)max_msgs; (void)max_chars;
    return msgs; /* identity — keep all messages */
}

char *split_thinking_content(const char *raw) { return str_dup(raw); }

int metrics_counter_inc(Metrics *m, const char *n, const char *h) { (void)m; (void)n; (void)h; return 0; }
int metrics_histogram_observe(Metrics *m, const char *n, const char *h, double v,
                                const double *b, int bn) { (void)m; (void)n; (void)h; (void)v; (void)b; (void)bn; return 0; }

/* safety.c is NOT in the build target for this test; provide the one symbol
 * agent.c references. Always returns 0 — agent_save_session never reaches the
 * approval path, so the actual value is irrelevant for this test. */
int safety_needs_approval(const SafetyConfig *cfg, const char *tool_name)
{ (void)cfg; (void)tool_name; return 0; }

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

    SessionManager *sm = session_manager_create(tmpdir, "pw");
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
    Session *s = session_manager_load_session(sm, agent.session_id);
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
    s = session_manager_load_session(sm, agent.session_id);
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

Suite *agent_suite(void)
{
    Suite *s = suite_create("Agent");
    TCase *tc = tcase_create("Core");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_agent_save_session_persists_empty_state);
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
