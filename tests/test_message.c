#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "agent/message.h"
#include "utils/string_utils.h"

START_TEST(test_message_create_free)
{
    Message *msg = message_create("user", "hello world");
    ck_assert_ptr_nonnull(msg);
    ck_assert_str_eq(msg->role, "user");
    ck_assert_str_eq(msg->content, "hello world");
    ck_assert(msg->timestamp > 0);
    message_free(msg);
}
END_TEST

START_TEST(test_message_tool_calls)
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

START_TEST(test_messages_to_json)
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

START_TEST(test_llm_response)
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

Suite *message_suite(void)
{
    Suite *s = suite_create("Message");
    TCase *tc = tcase_create("Core");
    tcase_add_test(tc, test_message_create_free);
    tcase_add_test(tc, test_message_tool_calls);
    tcase_add_test(tc, test_messages_to_json);
    tcase_add_test(tc, test_llm_response);
    tcase_add_test(tc, test_message_copy_deep);
    tcase_add_test(tc, test_message_copy_sparse);
    tcase_add_test(tc, test_message_copy_null_args);
    suite_add_tcase(s, tc);
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
