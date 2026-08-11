/* test_routes_ws_provider.c - WebSocket chat provider behavior tests
 * Split from test_routes_ws.c (2026-08 file-length compliance); shared
 * stubs and fixtures live in test_routes_ws_helpers.c. Depends on:
 * check, the routes_ws units under ROUTES_WS_TEST.
 */

#define _GNU_SOURCE

#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <cjson/cJSON.h>
#include <uv.h>

#include "test_routes_ws_helpers.h"

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


Suite *routes_ws_provider_suite(void)
{
    Suite *s = suite_create("routes_ws_provider");
    TCase *tc = tcase_create("ws_provider_config");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_on_message_provider_config);
    tcase_add_test(tc, test_on_message_provider_config_switches_provider);
    tcase_add_test(tc, test_on_message_provider_config_applies_effort_override);
    tcase_add_test(tc, test_on_message_provider_config_empty_effort_clears);
    tcase_add_test(tc, test_on_message_provider_config_rejects_invalid_effort);
    tcase_add_test(tc, test_on_message_provider_config_rejects_effort_for_unsupported_provider);
    tcase_add_test(tc, test_on_message_provider_config_opencode_zen_accepts_effort);
    tcase_add_test(tc, test_on_message_provider_config_openai_compatible_accepts_its_set);
    tcase_add_test(tc, test_on_message_provider_config_resolves_token_from_conf);
    tcase_add_test(tc, test_on_message_provider_switch_uses_target_default_base_url);
    tcase_add_test(tc, test_on_message_provider_switch_uses_conf_base_url_override);
    tcase_add_test(tc, test_on_message_provider_config_same_provider);
    tcase_add_test(tc, test_on_message_provider_config_failure_sends_error);
    tcase_add_test(tc, test_on_message_provider_config_model_only);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    Suite *s = routes_ws_provider_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed;
}
