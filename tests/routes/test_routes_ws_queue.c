/* test_routes_ws_queue.c - WebSocket chat queue behavior tests
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


Suite *routes_ws_queue_suite(void)
{
    Suite *s = suite_create("routes_ws_queue");
    TCase *tc = tcase_create("ws_chat_enqueue");
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

    tc = tcase_create("routes_ws_chat_init");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_ws_init_agent_create_fails);
    tcase_add_test(tc, test_ws_init_creates_agent_and_sends_ready);
    tcase_add_test(tc, test_ws_init_with_query_no_session_param);
    tcase_add_test(tc, test_ws_init_agent_has_session_id);
    tcase_add_test(tc, test_ws_init_with_session_id_query);
    tcase_add_test(tc, test_logout_invalidation_cancels_agents_and_releases_storage);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    Suite *s = routes_ws_queue_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed;
}
