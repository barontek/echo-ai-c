#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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

/* Mock symbols so agent.c links without dragging in the whole tool chain.
 * Same approach as test_agent_save.c (AGENTS.md §11.2): agent.c as a
 * translation unit references these, but the tests only exercise
 * agent_create/agent_set_provider/agent_destroy. */

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
void registry_set_ask_user_callback(char *(*cb)(const char *, void *),
                                    void *userdata)
{
    (void)cb; (void)userdata;
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
    return msgs;
}

char *split_thinking_content(const char *raw) { return str_dup(raw); }

int metrics_counter_inc(Metrics *m, const char *n, const char *h) { (void)m; (void)n; (void)h; return 0; }
int metrics_histogram_observe(Metrics *m, const char *n, const char *h, double v,
                                const double *b, int bn) { (void)m; (void)n; (void)h; (void)v; (void)b; (void)bn; return 0; }

int safety_needs_approval(const SafetyConfig *cfg, const char *tool_name)
{ (void)cfg; (void)tool_name; return 0; }

/* ---- Mock provider (get_provider) ---- */

typedef struct {
    LLMProvider provider;
    int generation;
} MockProvider;

static int mock_get_provider_fail_next = 0;
static int mock_get_provider_calls = 0;
static const char *mock_last_name = NULL;
static const char *mock_last_base_url = NULL;
static const char *mock_last_token = NULL;
static int mock_last_num_ctx = 0;
static int mock_last_keep_alive = 0;
static const char *mock_last_effort = NULL;
static int mock_generation = 0;
static int mock_destroy_count = 0;

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
static void mock_destroy(LLMProvider *self)
{
    mock_destroy_count++;
    free(self);
}

/* Replaces factory.c's get_provider in this test build (factory.c is not
 * linked). Records the last request so tests can assert what the agent
 * passed through, and can force a failure to exercise the error path. */
LLMProvider *get_provider(const char *name, const char *model, const char *base_url,
                           const char *api_token, int num_ctx, int keep_alive_secs,
                           const char *effort)
{
    (void)model;
    mock_get_provider_calls++;
    mock_last_name = name;
    mock_last_base_url = base_url;
    mock_last_token = api_token;
    mock_last_num_ctx = num_ctx;
    mock_last_keep_alive = keep_alive_secs;
    mock_last_effort = effort;
    if (mock_get_provider_fail_next)
    {
        mock_get_provider_fail_next = 0;
        return NULL;
    }
    MockProvider *mock = calloc(1, sizeof(MockProvider));
    if (!mock) return NULL;
    mock->provider.chat = mock_chat;
    mock->provider.chat_streaming = mock_chat_streaming;
    mock->provider.destroy = mock_destroy;
    mock->generation = ++mock_generation;
    return (LLMProvider *)mock;
}

static void reset_mock(void)
{
    mock_get_provider_fail_next = 0;
    mock_get_provider_calls = 0;
    mock_last_name = NULL;
    mock_last_base_url = NULL;
    mock_last_token = NULL;
    mock_last_num_ctx = 0;
    mock_last_keep_alive = 0;
    mock_last_effort = NULL;
    mock_generation = 0;
    mock_destroy_count = 0;
}

static int provider_generation(Agent *agent)
{
    MockProvider *mp = (MockProvider *)agent->provider;
    return mp->generation;
}

static AgentConfig make_cfg(const char *provider, const char *model)
{
    AgentConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider = provider;
    cfg.model = model;
    cfg.base_url = "http://localhost:11434";
    cfg.api_token = "sk-initial-token";
    cfg.system_prompt = "You are a helpful assistant.";
    cfg.temperature = 0.7;
    cfg.timeout = 120;
    cfg.max_iterations = 50;
    cfg.max_context_messages = 50;
    cfg.max_context_chars = 100000;
    cfg.num_ctx = 4096;
    cfg.keep_alive_secs = 120;
    cfg.parallel_tool_exec = 0;
    return cfg;
}

START_TEST(test_set_provider_swaps_provider)
{
    reset_mock();
    AgentConfig cfg = make_cfg("ollama", "llama3");
    Agent *agent = agent_create(&cfg);
    ck_assert_ptr_nonnull(agent);
    ck_assert_str_eq(agent->provider_name, "ollama");
    ck_assert_str_eq(agent->provider_token, "sk-initial-token");
    int gen_before = provider_generation(agent);

    int rc = agent_set_provider(agent, "openai", "http://localhost:1234",
                                "sk-switch-1", 2048, 30, "high");
    ck_assert_int_eq(rc, 0);
    ck_assert(provider_generation(agent) != gen_before);
    ck_assert_str_eq(agent->provider_name, "openai");
    ck_assert_str_eq(agent->provider_token, "sk-switch-1");
    ck_assert_int_eq(mock_get_provider_calls, 2);
    ck_assert_str_eq(mock_last_name, "openai");
    ck_assert_str_eq(mock_last_base_url, "http://localhost:1234");
    ck_assert_str_eq(mock_last_token, "sk-switch-1");
    ck_assert_int_eq(mock_last_num_ctx, 2048);
    ck_assert_int_eq(mock_last_keep_alive, 30);
    ck_assert_str_eq(mock_last_effort, "high");
    ck_assert_str_eq(agent->effort, "high");
    ck_assert_int_eq(mock_destroy_count, 1);

    agent_destroy(agent);
    ck_assert_int_eq(mock_destroy_count, 2);
}
END_TEST

START_TEST(test_set_provider_same_name_is_noop)
{
    reset_mock();
    AgentConfig cfg = make_cfg("ollama", "llama3");
    Agent *agent = agent_create(&cfg);
    ck_assert_ptr_nonnull(agent);
    LLMProvider *p_before = agent->provider;

    int rc = agent_set_provider(agent, "ollama", "http://other:9999", NULL, 1, 1, NULL);
    ck_assert_int_eq(rc, 0);
    ck_assert(agent->provider == p_before);
    ck_assert_int_eq(mock_get_provider_calls, 1);
    ck_assert_int_eq(mock_destroy_count, 0);
    ck_assert_ptr_null(agent->effort);

    agent_destroy(agent);
}
END_TEST

START_TEST(test_set_provider_same_name_different_effort_rebuilds)
{
    reset_mock();
    AgentConfig cfg = make_cfg("ollama", "llama3");
    Agent *agent = agent_create(&cfg);
    ck_assert_ptr_nonnull(agent);
    LLMProvider *p_before = agent->provider;

    /* Same provider name, but a new effort: effort is baked into the
     * provider at creation, so this must rebuild rather than no-op. */
    int rc = agent_set_provider(agent, "ollama", "http://localhost:11434",
                                "sk-initial-token", 4096, 120, "low");
    ck_assert_int_eq(rc, 0);
    ck_assert(agent->provider != p_before);
    ck_assert_int_eq(mock_get_provider_calls, 2);
    ck_assert_int_eq(mock_destroy_count, 1);
    ck_assert_str_eq(mock_last_effort, "low");
    ck_assert_str_eq(agent->effort, "low");

    /* And the reverse: same effort again is a no-op. */
    LLMProvider *p_after = agent->provider;
    rc = agent_set_provider(agent, "ollama", "http://localhost:11434",
                            "sk-initial-token", 4096, 120, "low");
    ck_assert_int_eq(rc, 0);
    ck_assert(agent->provider == p_after);
    ck_assert_int_eq(mock_get_provider_calls, 2);
    ck_assert_int_eq(mock_destroy_count, 1);

    agent_destroy(agent);
    ck_assert_int_eq(mock_destroy_count, 2);
}
END_TEST

START_TEST(test_set_provider_failure_keeps_old_provider)
{
    reset_mock();
    AgentConfig cfg = make_cfg("ollama", "llama3");
    Agent *agent = agent_create(&cfg);
    ck_assert_ptr_nonnull(agent);
    LLMProvider *p_before = agent->provider;

    mock_get_provider_fail_next = 1;
    int rc = agent_set_provider(agent, "openai", "http://localhost:1234",
                                "sk-switch-1", 2048, 30, "low");
    ck_assert_int_eq(rc, -1);
    ck_assert(agent->provider == p_before);
    ck_assert_str_eq(agent->provider_name, "ollama");
    ck_assert_ptr_null(agent->effort);
    ck_assert_int_eq(mock_destroy_count, 0);

    /* Agent still fully usable with the old provider. */
    agent_destroy(agent);
    ck_assert_int_eq(mock_destroy_count, 1);
}
END_TEST

START_TEST(test_set_provider_rejects_null_args)
{
    reset_mock();
    AgentConfig cfg = make_cfg("ollama", "llama3");
    Agent *agent = agent_create(&cfg);
    ck_assert_ptr_nonnull(agent);
    LLMProvider *p_before = agent->provider;

    ck_assert_int_eq(agent_set_provider(agent, NULL, "http://x", NULL, 1, 1, NULL), -1);
    ck_assert_int_eq(agent_set_provider(agent, "", "http://x", NULL, 1, 1, NULL), -1);
    ck_assert_int_eq(agent_set_provider(NULL, "ollama", "http://x", NULL, 1, 1, NULL), -1);
    ck_assert(agent->provider == p_before);
    ck_assert_int_eq(mock_get_provider_calls, 1);

    agent_destroy(agent);
}
END_TEST

START_TEST(test_set_provider_model_unchanged_by_switch)
{
    reset_mock();
    AgentConfig cfg = make_cfg("ollama", "llama3");
    Agent *agent = agent_create(&cfg);
    ck_assert_ptr_nonnull(agent);

    ck_assert_int_eq(agent_set_provider(agent, "openai", NULL, "sk-switch-2", 4096, 120, "medium"), 0);
    ck_assert_str_eq(agent->model, "llama3");
    ck_assert_str_eq(mock_last_token, "sk-switch-2");
    ck_assert_str_eq(mock_last_effort, "medium");

    agent_destroy(agent);
}
END_TEST

START_TEST(test_create_passes_effort_to_provider)
{
    reset_mock();
    AgentConfig cfg = make_cfg("ollama", "llama3");
    cfg.effort = "high";
    Agent *agent = agent_create(&cfg);
    ck_assert_ptr_nonnull(agent);
    ck_assert_str_eq(mock_last_effort, "high");
    ck_assert_str_eq(agent->effort, "high");

    agent_destroy(agent);
}
END_TEST

START_TEST(test_create_effort_null_means_unset)
{
    reset_mock();
    AgentConfig cfg = make_cfg("ollama", "llama3");
    cfg.effort = NULL;
    Agent *agent = agent_create(&cfg);
    ck_assert_ptr_nonnull(agent);
    ck_assert_ptr_null(mock_last_effort);
    ck_assert_ptr_null(agent->effort);

    agent_destroy(agent);
}
END_TEST

Suite *agent_provider_suite(void)
{
    Suite *s = suite_create("AgentProvider");
    TCase *tc = tcase_create("ProviderSwitch");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_set_provider_swaps_provider);
    tcase_add_test(tc, test_set_provider_same_name_is_noop);
    tcase_add_test(tc, test_set_provider_same_name_different_effort_rebuilds);
    tcase_add_test(tc, test_set_provider_failure_keeps_old_provider);
    tcase_add_test(tc, test_set_provider_rejects_null_args);
    tcase_add_test(tc, test_set_provider_model_unchanged_by_switch);
    tcase_add_test(tc, test_create_passes_effort_to_provider);
    tcase_add_test(tc, test_create_effort_null_means_unset);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = agent_provider_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
