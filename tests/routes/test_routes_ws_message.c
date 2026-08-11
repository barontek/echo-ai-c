/* test_routes_ws_message.c - WebSocket chat message behavior tests
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


Suite *routes_ws_message_suite(void)
{
    Suite *s = suite_create("routes_ws_message");
    TCase *tc = tcase_create("ws_chat_on_message");
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
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_message_run");
    tcase_add_checked_fixture(tc, setup, teardown);
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
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    Suite *s = routes_ws_message_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed;
}
