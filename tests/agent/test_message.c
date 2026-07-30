#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "agent/message.h"
#include "utils/string_utils.h"
#include "session/session.h"

START_TEST(test_message_create_initializes_role_content_and_timestamp)
{
    Message *msg = message_create("user", "hello world");
    ck_assert_ptr_nonnull(msg);
    ck_assert_str_eq(msg->role, "user");
    ck_assert_str_eq(msg->content, "hello world");
    ck_assert(msg->timestamp > 0);
    message_free(msg);
}
END_TEST

START_TEST(test_message_set_tool_calls_stores_count_and_name)
{
    Message *msg = message_create("assistant", "");
    ck_assert_ptr_nonnull(msg);

    ToolCall *tc = tool_call_create("call_1", "bash", "{\"command\":\"ls\"}");
    ck_assert_ptr_nonnull(tc);

    message_set_tool_calls(msg, tc, 1);
    ck_assert_int_eq(msg->tool_calls_count, 1);
    ck_assert_str_eq(msg->tool_calls[0].name, "bash");

    msg->tool_calls = NULL;
    msg->tool_calls_count = 0;
    tool_call_free(tc);
    free(tc);
    message_free(msg);
}
END_TEST

START_TEST(test_messages_to_json_array_emits_role_and_content_for_each_message)
{
    Message msgs[2];
    memset(msgs, 0, sizeof(msgs));

    msgs[0].role = str_dup("user");
    msgs[0].content = str_dup("hello");
    msgs[0].tool_call_id = NULL;
    msgs[0].tool_calls = NULL;
    msgs[0].tool_calls_count = 0;
    msgs[0].thinking = NULL;
    msgs[0].id = NULL;
    msgs[0].tool_name = NULL;
    msgs[0].error_category = NULL;

    msgs[1].role = str_dup("assistant");
    msgs[1].content = str_dup("hi there");
    msgs[1].tool_call_id = NULL;
    msgs[1].tool_calls = NULL;
    msgs[1].tool_calls_count = 0;
    msgs[1].thinking = NULL;
    msgs[1].id = NULL;
    msgs[1].tool_name = NULL;
    msgs[1].error_category = NULL;

    cJSON *arr = messages_to_json_array(msgs, 2);
    ck_assert_ptr_nonnull(arr);

    char *json = cJSON_PrintUnformatted(arr);
    ck_assert_ptr_nonnull(json);
    ck_assert(strstr(json, "\"role\":\"user\"") != NULL);
    ck_assert(strstr(json, "\"role\":\"assistant\"") != NULL);
    ck_assert(strstr(json, "\"hello\"") != NULL);
    ck_assert(strstr(json, "\"hi there\"") != NULL);

    free(json);
    cJSON_Delete(arr);

    free(msgs[0].role); free(msgs[0].content);
    free(msgs[1].role); free(msgs[1].content);
}
END_TEST

START_TEST(test_llm_response_create_initializes_content_and_tool_calls_to_null)
{
    LLMResponse *resp = llm_response_create();
    ck_assert_ptr_nonnull(resp);
    ck_assert_ptr_null(resp->content);
    ck_assert_ptr_null(resp->tool_calls);

    resp->content = str_dup("test response");
    llm_response_free(resp);
}
END_TEST

START_TEST(test_message_copy_deep)
{
    Message src;
    memset(&src, 0, sizeof(src));
    src.role = str_dup("assistant");
    src.content = str_dup("hello");
    src.id = str_dup("msg-1");
    src.thinking = str_dup("hmm");
    src.tool_call_id = str_dup("tc-1");
    src.tool_name = str_dup("bash");
    src.error_category = str_dup("none");
    src.timestamp = 123.5;
    src.tool_calls = calloc(1, sizeof(ToolCall));
    src.tool_calls_count = 1;
    src.tool_calls[0].id = str_dup("call-1");
    src.tool_calls[0].name = str_dup("bash");
    src.tool_calls[0].arguments = str_dup("{\"command\":\"ls\"}");
    src.tool_calls[0].result_content = str_dup("output");
    src.tool_calls[0].result_error = str_dup("warning");

    Message *dst = calloc(1, sizeof(Message));
    ck_assert_ptr_nonnull(dst);
    ck_assert_int_eq(message_copy(dst, &src), 0);

    /* contents equal, pointers distinct (true deep copy) */
    ck_assert_str_eq(dst->role, src.role);
    ck_assert_ptr_ne(dst->role, src.role);
    ck_assert_str_eq(dst->content, src.content);
    ck_assert_ptr_ne(dst->content, src.content);
    ck_assert_str_eq(dst->id, src.id);
    ck_assert_ptr_ne(dst->id, src.id);
    ck_assert_str_eq(dst->thinking, src.thinking);
    ck_assert_ptr_ne(dst->thinking, src.thinking);
    ck_assert_str_eq(dst->tool_call_id, src.tool_call_id);
    ck_assert_ptr_ne(dst->tool_call_id, src.tool_call_id);
    ck_assert_str_eq(dst->tool_name, src.tool_name);
    ck_assert_ptr_ne(dst->tool_name, src.tool_name);
    ck_assert_str_eq(dst->error_category, src.error_category);
    ck_assert_ptr_ne(dst->error_category, src.error_category);
    ck_assert_double_eq(dst->timestamp, src.timestamp);
    ck_assert_ptr_ne(dst->tool_calls, src.tool_calls);
    ck_assert_str_eq(dst->tool_calls[0].id, src.tool_calls[0].id);
    ck_assert_str_eq(dst->tool_calls[0].name, src.tool_calls[0].name);
    ck_assert_str_eq(dst->tool_calls[0].arguments, src.tool_calls[0].arguments);
    ck_assert_ptr_ne(dst->tool_calls[0].arguments, src.tool_calls[0].arguments);
    ck_assert_str_eq(dst->tool_calls[0].result_content,
                     src.tool_calls[0].result_content);
    ck_assert_ptr_ne(dst->tool_calls[0].result_content,
                     src.tool_calls[0].result_content);
    ck_assert_str_eq(dst->tool_calls[0].result_error,
                     src.tool_calls[0].result_error);

    /* regression: source must be left untouched (agent_save_session used
     * to hollow it out, which corrupted later saves and the live context) */
    ck_assert_str_eq(src.role, "assistant");
    ck_assert_str_eq(src.content, "hello");
    ck_assert_str_eq(src.tool_calls[0].name, "bash");

    /* the copy serializes with real content, not hollow fields */
    cJSON *arr = messages_to_json_array(dst, 1);
    ck_assert_ptr_nonnull(arr);
    char *json = cJSON_PrintUnformatted(arr);
    ck_assert_ptr_nonnull(json);
    ck_assert(strstr(json, "\"role\":\"assistant\"") != NULL);
    ck_assert(strstr(json, "\"hello\"") != NULL);
    free(json);
    cJSON_Delete(arr);

    message_free_all(dst, 1);

    free(src.role);
    free(src.content);
    free(src.id);
    free(src.thinking);
    free(src.tool_call_id);
    free(src.tool_name);
    free(src.error_category);
    tool_call_free(&src.tool_calls[0]);
    free(src.tool_calls);
}
END_TEST

START_TEST(test_message_copy_sparse)
{
    Message src;
    memset(&src, 0, sizeof(src));
    src.role = str_dup("user");
    src.content = str_dup("hi");

    Message *dst = calloc(1, sizeof(Message));
    ck_assert_ptr_nonnull(dst);
    ck_assert_int_eq(message_copy(dst, &src), 0);

    ck_assert_str_eq(dst->role, "user");
    ck_assert_str_eq(dst->content, "hi");
    ck_assert_ptr_null(dst->id);
    ck_assert_ptr_null(dst->thinking);
    ck_assert_ptr_null(dst->tool_calls);
    ck_assert_int_eq(dst->tool_calls_count, 0);

    message_free_all(dst, 1);
    free(src.role);
    free(src.content);
}
END_TEST

START_TEST(test_message_copy_null_args)
{
    Message m;
    memset(&m, 0, sizeof(m));
    ck_assert_int_eq(message_copy(NULL, &m), -1);
    ck_assert_int_eq(message_copy(&m, NULL), -1);
}
END_TEST

/* Regression test for A4: Message.timestamp is set by message_create but
 * used to be silently dropped by messages_to_json_array. After the fix, the
 * JSON contains a "timestamp" number, and round-tripping through
 * session_deserialize_messages restores the same value. On the old code this
 * test fails because the JSON has no timestamp key and the deserialized value
 * is 0.0. */
START_TEST(test_message_timestamp_roundtrip)
{
    Message src;
    memset(&src, 0, sizeof(src));
    src.role = str_dup("user");
    src.content = str_dup("hello");
    src.timestamp = 1735689600.123; /* fixed value so the assertion is exact */

    cJSON *arr = messages_to_json_array(&src, 1);
    ck_assert_ptr_nonnull(arr);
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    ck_assert_ptr_nonnull(json);
    ck_assert(strstr(json, "\"timestamp\":") != NULL);
    ck_assert(strstr(json, "1735689600") != NULL);

    Session *s = session_create(NULL);
    ck_assert_ptr_nonnull(s);
    ck_assert_int_eq(session_deserialize_messages(s, json), 0);
    ck_assert_int_eq(s->messages_count, 1);
    ck_assert(s->messages[0].timestamp > 0.0);
    ck_assert_double_eq_tol(s->messages[0].timestamp, 1735689600.123, 1.0);

    free(json);
    session_free(s);
    free(src.role);
    free(src.content);
}
END_TEST

/* Regression test for A5: Message.id was read by session_deserialize_messages
 * but never emitted by messages_to_json_array, so reload silently wiped the id
 * to NULL. After the fix the JSON contains an "id" key and the round-trip
 * restores it. On the old code this test fails because the JSON has no "id"
 * and the deserialized value is NULL. */
START_TEST(test_message_id_roundtrip)
{
    Message src;
    memset(&src, 0, sizeof(src));
    src.role = str_dup("assistant");
    src.content = str_dup("ok");
    src.id = str_dup("msg-abc-123");

    cJSON *arr = messages_to_json_array(&src, 1);
    ck_assert_ptr_nonnull(arr);
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    ck_assert_ptr_nonnull(json);
    ck_assert(strstr(json, "\"id\":\"msg-abc-123\"") != NULL);

    Session *s = session_create(NULL);
    ck_assert_ptr_nonnull(s);
    ck_assert_int_eq(session_deserialize_messages(s, json), 0);
    ck_assert_int_eq(s->messages_count, 1);
    ck_assert_ptr_nonnull(s->messages[0].id);
    ck_assert_str_eq(s->messages[0].id, "msg-abc-123");

    free(json);
    session_free(s);
    free(src.role);
    free(src.content);
    free(src.id);
}
END_TEST

/* Regression test for A6: ToolCall.result_content/result_error were populated
 * by execute_tool_calls and emitted via the WebSocket "done" frame, but never
 * persisted by messages_to_json_array, so a reloaded session lost all tool
 * results. After the fix the JSON carries result_content/result_error on each
 * tool_call and the deserializer restores them. On the old code this test
 * fails because the JSON has neither field. */
START_TEST(test_tool_call_result_roundtrip)
{
    Message *src = calloc(1, sizeof(Message));
    ck_assert_ptr_nonnull(src);
    src->role = str_dup("assistant");
    src->content = str_dup("");
    src->tool_calls = calloc(1, sizeof(ToolCall));
    ck_assert_ptr_nonnull(src->tool_calls);
    src->tool_calls_count = 1;
    src->tool_calls[0].id = str_dup("call_42");
    src->tool_calls[0].name = str_dup("bash");
    src->tool_calls[0].arguments = str_dup("{\"command\":\"ls\"}");
    src->tool_calls[0].result_content = str_dup("file1\nfile2");
    src->tool_calls[0].result_error = NULL;

    cJSON *arr = messages_to_json_array(src, 1);
    ck_assert_ptr_nonnull(arr);
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    ck_assert_ptr_nonnull(json);
    ck_assert(strstr(json, "\"result_content\":\"file1\\nfile2\"") != NULL);
    ck_assert(strstr(json, "\"result_error\"") == NULL);

    Session *s = session_create(NULL);
    ck_assert_ptr_nonnull(s);
    ck_assert_int_eq(session_deserialize_messages(s, json), 0);
    ck_assert_int_eq(s->messages_count, 1);
    ck_assert_int_eq(s->messages[0].tool_calls_count, 1);
    ck_assert_ptr_nonnull(s->messages[0].tool_calls[0].result_content);
    ck_assert_str_eq(s->messages[0].tool_calls[0].result_content, "file1\nfile2");
    ck_assert_ptr_null(s->messages[0].tool_calls[0].result_error);

    free(json);
    session_free(s);
    message_free_all(src, 1);
}
END_TEST

/* Regression test for C1: session_deserialize_messages used to overwrite
 * session->messages without freeing the prior array — a memory leak. After
 * the fix it calls message_free_all first (same pattern as the
 * metadata/events deserializers). The test populates a session with one
 * message, then deserializes a new JSON array into the same session, and
 * asserts the new messages replaced the old ones (count=1 matching the new
 * JSON, not 2). ASan's leak detector also catches the leaked old array. */
START_TEST(test_deserialize_replaces_prior_messages)
{
    Session *s = session_create(NULL);
    ck_assert_ptr_nonnull(s);

    s->messages = calloc(1, sizeof(Message));
    ck_assert_ptr_nonnull(s->messages);
    s->messages_count = 1;
    s->messages[0].role = str_dup("user");
    s->messages[0].content = str_dup("old message that should be freed");

    const char *new_json = "[{\"role\":\"assistant\",\"content\":\"new\"}]";
    ck_assert_int_eq(session_deserialize_messages(s, new_json), 0);
    ck_assert_int_eq(s->messages_count, 1);
    ck_assert_str_eq(s->messages[0].role, "assistant");
    ck_assert_str_eq(s->messages[0].content, "new");

    session_free(s);
}
END_TEST

/* C15 regression: previously every per-Message teardown was open-coded as an
 * inline loop in 4 different sites (session.c:55-69, agent.c:67-81,
 * agent.c:355-361, routes.c:1366-1380). The agent.c:355-361 site additionally
 * FORGOT to free `msgs[i].tool_calls` and each `tool_calls[j]`'s inner strings
 * — a real leak affecting every chat turn where apply_context_window actually
 * shrank the message list. The C15 fix extracts `message_clear()` as the
 * single source of truth and wires every per-Message teardown through it.
 *
 * This test constructs a Message that owns every optional field INCLUDING two
 * tool_calls (each owning its own id/name/arguments/result strings), calls
 * `message_clear` once, then frees the surrounding allocation, and asserts
 * (under ASan) that nothing leaks. On the OLD `agent.c:355-361` pattern
 * (which inlined a per-Message loop without the tool_calls block), running
 * this exact Message through a `message_free_all`-equivalent would leak
 * every ToolCall — but we cannot replicate the old loop inside this test
 * without open-coding it, which defeats the point. The test instead proves
 * `message_clear` itself is correct; the failure-mode for the refactor's
 * correctness is "if message_clear ever drops the tool_calls block, this
 * test starts leaking under ASan". */
START_TEST(test_message_clear_frees_all_fields_incl_tool_calls)
{
    Message *msg = message_create("assistant", "content with tool calls");
    ck_assert_ptr_nonnull(msg);
    msg->id = str_dup("msg-id-1");
    msg->tool_call_id = str_dup("tool_call_id-1");
    msg->tool_name = str_dup("bash");
    msg->error_category = str_dup("network");
    msg->thinking = str_dup("Considering options...");

    ToolCall *tc1 = tool_call_create("tcall-1", "bash", "{\"cmd\":\"ls\"}");
    ck_assert_ptr_nonnull(tc1);
    tc1->result_content = str_dup("file1\nfile2");
    tc1->result_error = str_dup("warn");

    ToolCall *tc2 = tool_call_create("tcall-2", "grep", "{\"pat\":\"foo\"}");
    ck_assert_ptr_nonnull(tc2);
    tc2->result_content = str_dup("foo: bar");

    /* Build a small tool_calls array (Take manual ownership; message_set_tool_calls
     * would also work but pulls in too much surrounding code). */
    ToolCall *calls = calloc(2, sizeof(ToolCall));
    ck_assert_ptr_nonnull(calls);
    calls[0] = *tc1; free(tc1);
    calls[1] = *tc2; free(tc2);
    message_set_tool_calls(msg, calls, 2);

    /* The single clear+free pair — if message_clear drops any field,
     * ASan detect_leaks will flag it. */
    message_clear(msg);
    free(msg);
}
END_TEST

Suite *message_suite(void)
{
    Suite *s = suite_create("Message");

    TCase *tc_lifecycle = tcase_create("Lifecycle");
    tcase_add_test(tc_lifecycle, test_message_create_initializes_role_content_and_timestamp);
    tcase_add_test(tc_lifecycle, test_message_set_tool_calls_stores_count_and_name);
    tcase_add_test(tc_lifecycle, test_llm_response_create_initializes_content_and_tool_calls_to_null);
    tcase_add_test(tc_lifecycle, test_message_clear_frees_all_fields_incl_tool_calls);
    suite_add_tcase(s, tc_lifecycle);

    TCase *tc_serialization = tcase_create("Serialization");
    tcase_add_test(tc_serialization, test_messages_to_json_array_emits_role_and_content_for_each_message);
    tcase_add_test(tc_serialization, test_message_timestamp_roundtrip);
    tcase_add_test(tc_serialization, test_message_id_roundtrip);
    tcase_add_test(tc_serialization, test_tool_call_result_roundtrip);
    tcase_add_test(tc_serialization, test_deserialize_replaces_prior_messages);
    suite_add_tcase(s, tc_serialization);

    TCase *tc_copy = tcase_create("Copy");
    tcase_add_test(tc_copy, test_message_copy_deep);
    tcase_add_test(tc_copy, test_message_copy_sparse);
    tcase_add_test(tc_copy, test_message_copy_null_args);
    suite_add_tcase(s, tc_copy);

    return s;
}

int main(void)
{
    Suite *s = message_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
