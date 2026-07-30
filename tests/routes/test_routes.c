#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "../src/agent/message.h"
#include "../src/server/routes/routes.h"

/* ---------------------------------------------------------------------------
 * route_match
 * --------------------------------------------------------------------------- */

START_TEST(test_route_match_method_mismatch)
{
    Route r = {"POST", "/api/status", 0, 0, 0, NULL};
    ck_assert_int_eq(route_match("GET", "/api/status", &r), 0);
}
END_TEST

START_TEST(test_route_match_exact_match)
{
    Route r = {"GET", "/api/status", 0, 0, 0, NULL};
    ck_assert_int_eq(route_match("GET", "/api/status", &r), 1);
}
END_TEST

START_TEST(test_route_match_prefix_match)
{
    Route r = {"GET", "/api/sessions/", 1, 1, 0, NULL};
    ck_assert_int_eq(route_match("GET", "/api/sessions/abc-def", &r), 1);
}
END_TEST

START_TEST(test_route_match_prefix_exact_treated_as_prefix)
{
    Route r = {"GET", "/api/sessions/", 1, 1, 0, NULL};
    ck_assert_int_eq(route_match("GET", "/api/sessions/", &r), 1);
}
END_TEST

START_TEST(test_route_match_prefix_no_match)
{
    Route r = {"GET", "/api/sessions/", 1, 1, 0, NULL};
    ck_assert_int_eq(route_match("GET", "/api/other", &r), 0);
}
END_TEST

START_TEST(test_route_match_non_prefix_no_match_different_path)
{
    Route r = {"GET", "/api/status", 0, 0, 0, NULL};
    ck_assert_int_eq(route_match("GET", "/api/health", &r), 0);
}
END_TEST

START_TEST(test_route_match_method_correct_path_wrong)
{
    Route r = {"DELETE", "/api/sessions/", 1, 1, 0, NULL};
    ck_assert_int_eq(route_match("PUT", "/api/sessions/abc", &r), 0);
}
END_TEST

START_TEST(test_route_match_prefix_truncation)
{
    /* When is_prefix=1, strncmp(path, r->path, strlen(r->path)) compares
     * only up to the stored prefix length, so "/api/prefix" does match
     * "/api/prefixed" (first 11 chars are identical). */
    Route r = {"GET", "/api/prefix", 1, 0, 0, NULL};
    ck_assert_int_eq(route_match("GET", "/api/prefix-more", &r), 1);
    ck_assert_int_eq(route_match("GET", "/api/prefix", &r), 1);
    ck_assert_int_eq(route_match("GET", "/api/other", &r), 0);
}
END_TEST

/* ---------------------------------------------------------------------------
 * ws_add_message_to_json
 * --------------------------------------------------------------------------- */

START_TEST(test_ws_add_message_to_json_basic)
{
    Message msg = {0};
    msg.role = "user";
    msg.content = "hello";

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);
    cJSON *role = cJSON_GetObjectItem(obj, "role");
    cJSON *content = cJSON_GetObjectItem(obj, "content");
    ck_assert_ptr_nonnull(role);
    ck_assert_str_eq(role->valuestring, "user");
    ck_assert_ptr_nonnull(content);
    ck_assert_str_eq(content->valuestring, "hello");
    ck_assert_ptr_null(cJSON_GetObjectItem(obj, "thinking"));
    ck_assert_ptr_null(cJSON_GetObjectItem(obj, "tool_calls"));
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_ws_add_message_to_json_null_role)
{
    Message msg = {0};
    msg.role = NULL;
    msg.content = "test";

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);
    cJSON *role = cJSON_GetObjectItem(obj, "role");
    ck_assert_ptr_nonnull(role);
    ck_assert_str_eq(role->valuestring, "unknown");
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_ws_add_message_to_json_null_content)
{
    Message msg = {0};
    msg.role = "assistant";
    msg.content = NULL;

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);
    cJSON *content = cJSON_GetObjectItem(obj, "content");
    ck_assert_ptr_nonnull(content);
    ck_assert_str_eq(content->valuestring, "");
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_ws_add_message_to_json_with_thinking)
{
    Message msg = {0};
    msg.role = "assistant";
    msg.content = "answer";
    msg.thinking = "thinking text";

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);
    cJSON *thinking = cJSON_GetObjectItem(obj, "thinking");
    ck_assert_ptr_nonnull(thinking);
    ck_assert_str_eq(thinking->valuestring, "thinking text");
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_ws_add_message_to_json_with_tool_name)
{
    Message msg = {0};
    msg.role = "tool";
    msg.content = "result";
    msg.tool_name = "hammer";

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);
    cJSON *tn = cJSON_GetObjectItem(obj, "tool_name");
    ck_assert_ptr_nonnull(tn);
    ck_assert_str_eq(tn->valuestring, "hammer");
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_ws_add_message_to_json_with_tool_call_id)
{
    Message msg = {0};
    msg.role = "tool";
    msg.content = "result";
    msg.tool_call_id = "call_123";

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);
    cJSON *tci = cJSON_GetObjectItem(obj, "tool_call_id");
    ck_assert_ptr_nonnull(tci);
    ck_assert_str_eq(tci->valuestring, "call_123");
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_ws_add_message_to_json_with_error_category)
{
    Message msg = {0};
    msg.role = "tool";
    msg.content = "err";
    msg.error_category = "network";

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);
    cJSON *ec = cJSON_GetObjectItem(obj, "error_category");
    ck_assert_ptr_nonnull(ec);
    ck_assert_str_eq(ec->valuestring, "network");
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_ws_add_message_to_json_with_tool_calls)
{
    ToolCall calls[2] = {0};
    calls[0].name = "read_file";
    calls[0].arguments = "{\"path\":\"/tmp\"}";
    calls[1].name = "write_file";
    calls[1].arguments = "{\"path\":\"/out\"}";

    Message msg = {0};
    msg.role = "assistant";
    msg.content = "doing work";
    msg.tool_calls = calls;
    msg.tool_calls_count = 2;

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);

    cJSON *tc_arr = cJSON_GetObjectItem(obj, "tool_calls");
    ck_assert_ptr_nonnull(tc_arr);
    ck_assert(cJSON_IsArray(tc_arr));
    ck_assert_int_eq(cJSON_GetArraySize(tc_arr), 2);

    cJSON *tc0 = cJSON_GetArrayItem(tc_arr, 0);
    ck_assert_str_eq(cJSON_GetObjectItem(tc0, "name")->valuestring, "read_file");
    ck_assert_str_eq(cJSON_GetObjectItem(tc0, "arguments")->valuestring, "{\"path\":\"/tmp\"}");

    cJSON *tc1 = cJSON_GetArrayItem(tc_arr, 1);
    ck_assert_str_eq(cJSON_GetObjectItem(tc1, "name")->valuestring, "write_file");

    ck_assert(cJSON_IsTrue(cJSON_GetObjectItem(obj, "has_tools")));
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_ws_add_message_to_json_zero_tool_calls)
{
    ToolCall calls[1] = {0};
    Message msg = {0};
    msg.role = "assistant";
    msg.content = "ok";
    msg.tool_calls = calls;
    msg.tool_calls_count = 0;

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);
    ck_assert_ptr_null(cJSON_GetObjectItem(obj, "tool_calls"));
    ck_assert_ptr_null(cJSON_GetObjectItem(obj, "has_tools"));
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_ws_add_message_to_json_tool_calls_null_name)
{
    ToolCall calls[1] = {0};
    calls[0].name = NULL;
    calls[0].arguments = "{}";

    Message msg = {0};
    msg.role = "assistant";
    msg.content = "ok";
    msg.tool_calls = calls;
    msg.tool_calls_count = 1;

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);

    cJSON *tc_arr = cJSON_GetObjectItem(obj, "tool_calls");
    cJSON *tc0 = cJSON_GetArrayItem(tc_arr, 0);
    ck_assert_str_eq(cJSON_GetObjectItem(tc0, "name")->valuestring, "");
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_ws_add_message_to_json_tool_calls_null_arguments)
{
    ToolCall calls[1] = {0};
    calls[0].name = "test";
    calls[0].arguments = NULL;

    Message msg = {0};
    msg.role = "assistant";
    msg.content = "ok";
    msg.tool_calls = calls;
    msg.tool_calls_count = 1;

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);

    cJSON *tc_arr = cJSON_GetObjectItem(obj, "tool_calls");
    cJSON *tc0 = cJSON_GetArrayItem(tc_arr, 0);
    ck_assert_str_eq(cJSON_GetObjectItem(tc0, "arguments")->valuestring, "{}");
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_ws_add_message_to_json_tool_calls_with_results)
{
    ToolCall calls[1] = {0};
    calls[0].name = "search";
    calls[0].arguments = "{\"q\":\"test\"}";
    calls[0].result_content = "found 3 results";
    calls[0].result_error = NULL;

    Message msg = {0};
    msg.role = "assistant";
    msg.content = "results";
    msg.tool_calls = calls;
    msg.tool_calls_count = 1;

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);

    cJSON *tc_arr = cJSON_GetObjectItem(obj, "tool_calls");
    cJSON *tc0 = cJSON_GetArrayItem(tc_arr, 0);
    ck_assert_str_eq(cJSON_GetObjectItem(tc0, "result_content")->valuestring, "found 3 results");
    ck_assert_ptr_null(cJSON_GetObjectItem(tc0, "result_error"));
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_ws_add_message_to_json_tool_calls_with_error)
{
    ToolCall calls[1] = {0};
    calls[0].name = "search";
    calls[0].arguments = "{}";
    calls[0].result_content = NULL;
    calls[0].result_error = "timeout";

    Message msg = {0};
    msg.role = "assistant";
    msg.content = "err";
    msg.tool_calls = calls;
    msg.tool_calls_count = 1;

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);

    cJSON *tc_arr = cJSON_GetObjectItem(obj, "tool_calls");
    cJSON *tc0 = cJSON_GetArrayItem(tc_arr, 0);
    ck_assert_str_eq(cJSON_GetObjectItem(tc0, "result_error")->valuestring, "timeout");
    ck_assert_ptr_null(cJSON_GetObjectItem(tc0, "result_content"));
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_ws_add_message_to_json_tool_calls_empty_results_not_emitted)
{
    ToolCall calls[1] = {0};
    calls[0].name = "noop";
    calls[0].arguments = "{}";
    calls[0].result_content = "";
    calls[0].result_error = "";

    Message msg = {0};
    msg.role = "assistant";
    msg.content = "ok";
    msg.tool_calls = calls;
    msg.tool_calls_count = 1;

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);

    cJSON *tc_arr = cJSON_GetObjectItem(obj, "tool_calls");
    cJSON *tc0 = cJSON_GetArrayItem(tc_arr, 0);
    /* Empty strings: result_content[0] == '\0', so condition fails */
    ck_assert_ptr_null(cJSON_GetObjectItem(tc0, "result_content"));
    ck_assert_ptr_null(cJSON_GetObjectItem(tc0, "result_error"));
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_ws_add_message_to_json_all_fields)
{
    ToolCall calls[1] = {0};
    calls[0].name = "multi";
    calls[0].arguments = "{\"a\":1}";
    calls[0].result_content = "done";

    Message msg = {0};
    msg.role = "assistant";
    msg.content = "full response";
    msg.thinking = "let me think";
    msg.tool_name = "multi_tool";
    msg.tool_call_id = "tc_999";
    msg.error_category = "none";
    msg.tool_calls = calls;
    msg.tool_calls_count = 1;

    cJSON *obj = cJSON_CreateObject();
    ws_add_message_to_json(obj, &msg);

    ck_assert_str_eq(cJSON_GetObjectItem(obj, "role")->valuestring, "assistant");
    ck_assert_str_eq(cJSON_GetObjectItem(obj, "content")->valuestring, "full response");
    ck_assert_str_eq(cJSON_GetObjectItem(obj, "thinking")->valuestring, "let me think");
    ck_assert_str_eq(cJSON_GetObjectItem(obj, "tool_name")->valuestring, "multi_tool");
    ck_assert_str_eq(cJSON_GetObjectItem(obj, "tool_call_id")->valuestring, "tc_999");
    ck_assert_str_eq(cJSON_GetObjectItem(obj, "error_category")->valuestring, "none");
    ck_assert_ptr_nonnull(cJSON_GetObjectItem(obj, "tool_calls"));
    cJSON_Delete(obj);
}
END_TEST

/* ---------------------------------------------------------------------------
 * Suite
 * --------------------------------------------------------------------------- */

Suite *routes_suite(void)
{
    Suite *s = suite_create("routes");

    TCase *tc_match = tcase_create("route_match");
    tcase_add_test(tc_match, test_route_match_method_mismatch);
    tcase_add_test(tc_match, test_route_match_exact_match);
    tcase_add_test(tc_match, test_route_match_prefix_match);
    tcase_add_test(tc_match, test_route_match_prefix_exact_treated_as_prefix);
    tcase_add_test(tc_match, test_route_match_prefix_no_match);
    tcase_add_test(tc_match, test_route_match_non_prefix_no_match_different_path);
    tcase_add_test(tc_match, test_route_match_method_correct_path_wrong);
    tcase_add_test(tc_match, test_route_match_prefix_truncation);
    suite_add_tcase(s, tc_match);

    TCase *tc_json = tcase_create("ws_add_message_to_json");
    tcase_add_test(tc_json, test_ws_add_message_to_json_basic);
    tcase_add_test(tc_json, test_ws_add_message_to_json_null_role);
    tcase_add_test(tc_json, test_ws_add_message_to_json_null_content);
    tcase_add_test(tc_json, test_ws_add_message_to_json_with_thinking);
    tcase_add_test(tc_json, test_ws_add_message_to_json_with_tool_name);
    tcase_add_test(tc_json, test_ws_add_message_to_json_with_tool_call_id);
    tcase_add_test(tc_json, test_ws_add_message_to_json_with_error_category);
    tcase_add_test(tc_json, test_ws_add_message_to_json_with_tool_calls);
    tcase_add_test(tc_json, test_ws_add_message_to_json_zero_tool_calls);
    tcase_add_test(tc_json, test_ws_add_message_to_json_tool_calls_null_name);
    tcase_add_test(tc_json, test_ws_add_message_to_json_tool_calls_null_arguments);
    tcase_add_test(tc_json, test_ws_add_message_to_json_tool_calls_with_results);
    tcase_add_test(tc_json, test_ws_add_message_to_json_tool_calls_with_error);
    tcase_add_test(tc_json, test_ws_add_message_to_json_tool_calls_empty_results_not_emitted);
    tcase_add_test(tc_json, test_ws_add_message_to_json_all_fields);
    suite_add_tcase(s, tc_json);

    return s;
}

int main(void)
{
    int failures = 0;
    Suite *s = routes_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_ENV);
    failures = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failures;
}
