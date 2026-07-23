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

Suite *message_suite(void)
{
    Suite *s = suite_create("Message");
    TCase *tc = tcase_create("Core");
    tcase_add_test(tc, test_message_create_free);
    tcase_add_test(tc, test_message_tool_calls);
    tcase_add_test(tc, test_messages_to_json);
    tcase_add_test(tc, test_llm_response);
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
