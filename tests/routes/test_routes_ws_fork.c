/* test_routes_ws_fork.c - WebSocket chat fork behavior tests
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


Suite *routes_ws_fork_suite(void)
{
    Suite *s = suite_create("routes_ws_fork");
    TCase *tc = tcase_create("ws_edit_regenerate");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_on_message_edit_disconnect_during_run_is_safe);
    tcase_add_test(tc, test_on_message_edit_forks);
    tcase_add_test(tc, test_on_message_edit_clears_tail_then_appends_fork);
    tcase_add_test(tc, test_on_message_regenerate_forks_at_previous_user_turn);
    tcase_add_test(tc, test_on_message_regenerate_invalid_index_errors);
    tcase_add_test(tc, test_on_message_edit_rejects_negative_index);
    tcase_add_test(tc, test_on_message_edit_rejects_huge_index);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    tc = tcase_create("ws_branch_switch");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_on_message_branch_switch_success);
    tcase_add_test(tc, test_on_message_branch_switch_not_found);
    tcase_add_test(tc, test_on_message_branch_info_request);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    Suite *s = routes_ws_fork_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed;
}
