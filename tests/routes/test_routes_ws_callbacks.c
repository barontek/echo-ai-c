/* test_routes_ws_callbacks.c - WebSocket chat callbacks behavior tests
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


Suite *routes_ws_callbacks_suite(void)
{
    Suite *s = suite_create("routes_ws_callbacks");
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
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_ask_user_cb_null_ctx);
    tcase_add_test(tc, test_ask_user_cb_null_ws);
    tcase_add_test(tc, test_ask_user_cb_with_answer);
    tcase_add_test(tc, test_ask_user_cb_null_question);
    tcase_add_test(tc, test_ask_user_cb_times_out_without_response);
    tcase_add_test(tc, test_ask_user_cb_default_timeout_never_nulls);
    tcase_set_timeout(tc, 60);
    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    Suite *s = routes_ws_callbacks_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed;
}
