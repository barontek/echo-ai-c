#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "../src/server/routes/routes.h"
#include "../src/server/routes/routes_chat.h"
#include "../src/agent/agent.h"
#include "../src/agent/message.h"
#include "../src/utils/string_utils.h"

/* test_routes_chat - unit tests for routes chat. Depends on: check, the module under test. */
static int stub_unlock = 1;
static const char *stub_query_token_match = NULL;
static LLMResponse *stub_agent_run_resp = NULL;
static LLMResponse *stub_agent_run_streaming_resp = NULL;
static int stub_streaming_chunk_count = 0;
static const char *stub_streaming_chunks[4] = {NULL};
static int captured_status = 0;
static char *captured_body = NULL;
static char captured_sse_buf[4096] = {0};
static int captured_sse_count = 0;
static int captured_close_count = 0;

static LLMResponse fake_resp_basic = {0};
static LLMResponse fake_resp_with_thinking = {0};
static LLMResponse fake_resp_with_tools = {0};
static LLMResponse fake_resp_null_content = {0};
static LLMResponse fake_resp_tool_nulls = {0};
static ToolCall fake_tool_calls[2] = {0};
static ToolCall fake_tool_calls_nulls[1] = {0};

static void reset_stubs(void)
{
    stub_unlock = 1;
    stub_query_token_match = NULL;
    stub_agent_run_resp = NULL;
    stub_agent_run_streaming_resp = NULL;
    stub_streaming_chunk_count = 0;
    for (int i = 0; i < 4; i++) stub_streaming_chunks[i] = NULL;
    captured_status = 0;
    free(captured_body); captured_body = NULL;
    memset(captured_sse_buf, 0, sizeof(captured_sse_buf));
    captured_sse_count = 0;
    captured_close_count = 0;
}

static void init_fake_resps(void)
{
    fake_resp_basic.content = "Hello, world!";
    fake_resp_basic.thinking = NULL;
    fake_resp_basic.tool_calls = NULL;
    fake_resp_basic.tool_calls_count = 0;

    fake_resp_with_thinking.content = "Result";
    fake_resp_with_thinking.thinking = "Let me think...";
    fake_resp_with_thinking.tool_calls = NULL;
    fake_resp_with_thinking.tool_calls_count = 0;

    fake_tool_calls[0].name = "search";
    fake_tool_calls[0].arguments = "{\"q\":\"test\"}";
    fake_tool_calls[0].result_content = NULL;
    fake_tool_calls[0].result_error = NULL;
    fake_tool_calls[1].name = "read_file";
    fake_tool_calls[1].arguments = "{\"path\":\"/tmp/x\"}";
    fake_tool_calls[1].result_content = NULL;
    fake_tool_calls[1].result_error = NULL;
    fake_resp_with_tools.content = "Using tools...";
    fake_resp_with_tools.thinking = NULL;
    fake_resp_with_tools.tool_calls = fake_tool_calls;
    fake_resp_with_tools.tool_calls_count = 2;

    fake_resp_null_content.content = NULL;
    fake_resp_null_content.thinking = NULL;
    fake_resp_null_content.tool_calls = NULL;
    fake_resp_null_content.tool_calls_count = 0;

    fake_tool_calls_nulls[0].name = NULL;
    fake_tool_calls_nulls[0].arguments = NULL;
    fake_tool_calls_nulls[0].result_content = NULL;
    fake_tool_calls_nulls[0].result_error = NULL;
    fake_resp_tool_nulls.content = "Done";
    fake_resp_tool_nulls.thinking = NULL;
    fake_resp_tool_nulls.tool_calls = fake_tool_calls_nulls;
    fake_resp_tool_nulls.tool_calls_count = 1;
}

static void setup(void)
{
    reset_stubs();
    init_fake_resps();
}

static void teardown(void)
{
    reset_stubs();
}

int middleware_check_unlock(HTTPRequest *req, ServerContext *ctx)
{
    (void)req; (void)ctx;
    return stub_unlock;
}

int middleware_check_unlock_query(HTTPRequest *req, ServerContext *ctx)
{
    (void)ctx;
    if (stub_unlock) return 1;
    if (!req->query[0]) return 0;
    if (stub_query_token_match)
    {
        const char *p = strstr(req->query, "token=");
        if (p)
        {
            p += 6;
            size_t len = 0;
            while (p[len] != '&' && p[len] != '\0') len++;
            if (len == strlen(stub_query_token_match) &&
                memcmp(p, stub_query_token_match, len) == 0)
                return 1;
        }
    }
    return 0;
}

void client_close(Client *client) { (void)client; captured_close_count++; }

int server_response(Client *client, int status, const char *content_type,
                     const char *body)
{
    (void)client; (void)content_type;
    captured_status = status;
    free(captured_body);
    captured_body = body ? str_dup(body) : NULL;
    return 0;
}

int server_response_json(Client *client, int status, const char *json)
{
    server_response(client, status, "application/json", json);
    return 0;
}

int server_response_error(Client *client, int status, const char *msg)
{
    (void)client;
    captured_status = status;
    free(captured_body);
    captured_body = msg ? str_dup(msg) : NULL;
    return 0;
}

int server_sse_write(Client *client, const char *data)
{
    (void)client;
    if (!data) return -1;
    size_t existing = strlen(captured_sse_buf);
    size_t remain = sizeof(captured_sse_buf) - existing - 1;
    if (remain > 0) strncat(captured_sse_buf, data, remain);
    captured_sse_count++;
    return 0;
}

LLMResponse *agent_run_new(Agent *agent, const char *user_input)
{
    (void)agent; (void)user_input;
    return stub_agent_run_resp;
}

LLMResponse *agent_run_streaming_new(Agent *agent, const char *user_input,
                                  void (*on_chunk)(const char *, void *),
                                  void *userdata)
{
    (void)agent; (void)user_input;
    for (int i = 0; i < stub_streaming_chunk_count; i++)
    {
        if (on_chunk)
            on_chunk(stub_streaming_chunks[i], userdata);
    }
    return stub_agent_run_streaming_resp;
}

void llm_response_free(LLMResponse *resp) { (void)resp; }

int metrics_counter_inc(Metrics *m, const char *name, const char *help)
{
    (void)m; (void)name; (void)help;
    return 0;
}

/* ---------------------------------------------------------------------------
 * handle_chat tests
 * --------------------------------------------------------------------------- */

START_TEST(test_chat_no_unlock)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    stub_unlock = 0;
    handle_chat(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 401);
    ck_assert_str_eq(captured_body, "unauthorized");
}
END_TEST

START_TEST(test_chat_no_body)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    req.body = NULL;
    req.body_len = 0;
    handle_chat(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);
    ck_assert_str_eq(captured_body, "missing message");
}
END_TEST

START_TEST(test_chat_empty_body)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    req.body = "";
    req.body_len = 0;
    handle_chat(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);
    ck_assert_str_eq(captured_body, "missing message");
}
END_TEST

START_TEST(test_chat_invalid_json)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    req.body = "not json";
    req.body_len = 8;
    handle_chat(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);
    ck_assert_str_eq(captured_body, "invalid json");
}
END_TEST

START_TEST(test_chat_missing_message)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    req.body = "{\"prompt\":\"hello\"}";
    req.body_len = 19;
    handle_chat(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);
    ck_assert_str_eq(captured_body, "missing message field");
}
END_TEST

START_TEST(test_chat_message_null)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    req.body = "{\"message\":null}";
    req.body_len = 17;
    handle_chat(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);
    ck_assert_str_eq(captured_body, "missing message field");
}
END_TEST

START_TEST(test_chat_no_agent)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    req.body = "{\"message\":\"hello\"}";
    req.body_len = 19;
    ctx.agent = NULL;
    handle_chat(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);
    ck_assert_str_eq(captured_body, "agent not initialized");
}
END_TEST

START_TEST(test_chat_agent_returns_null)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    req.body = "{\"message\":\"hello\"}";
    req.body_len = 19;
    ctx.agent = (Agent *)&ctx;
    stub_agent_run_resp = NULL;
    handle_chat(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);
    ck_assert_str_eq(captured_body, "agent returned no response");
}
END_TEST

START_TEST(test_chat_returns_200_with_assistant_response)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    req.body = "{\"message\":\"hello\"}";
    req.body_len = 19;
    ctx.agent = (Agent *)&ctx;
    stub_agent_run_resp = &fake_resp_basic;
    handle_chat(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    cJSON *resp = cJSON_Parse(captured_body);
    ck_assert_ptr_nonnull(resp);
    cJSON *content = cJSON_GetObjectItem(resp, "content");
    ck_assert_ptr_nonnull(content);
    ck_assert_str_eq(content->valuestring, "Hello, world!");
    cJSON *has_tools = cJSON_GetObjectItem(resp, "has_tools");
    ck_assert_ptr_nonnull(has_tools);
    ck_assert_int_eq(has_tools->valueint, 0);
    ck_assert_ptr_null(cJSON_GetObjectItem(resp, "thinking"));
    cJSON_Delete(resp);
}
END_TEST

START_TEST(test_chat_success_with_thinking)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    req.body = "{\"message\":\"hello\"}";
    req.body_len = 19;
    ctx.agent = (Agent *)&ctx;
    stub_agent_run_resp = &fake_resp_with_thinking;
    handle_chat(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    cJSON *resp = cJSON_Parse(captured_body);
    ck_assert_ptr_nonnull(resp);
    cJSON *content = cJSON_GetObjectItem(resp, "content");
    ck_assert_str_eq(content->valuestring, "Result");
    cJSON *thinking = cJSON_GetObjectItem(resp, "thinking");
    ck_assert_ptr_nonnull(thinking);
    ck_assert_str_eq(thinking->valuestring, "Let me think...");
    cJSON_Delete(resp);
}
END_TEST

START_TEST(test_chat_success_with_tool_calls)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    req.body = "{\"message\":\"search for python\"}";
    req.body_len = 33;
    ctx.agent = (Agent *)&ctx;
    stub_agent_run_resp = &fake_resp_with_tools;
    handle_chat(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    cJSON *resp = cJSON_Parse(captured_body);
    ck_assert_ptr_nonnull(resp);
    cJSON *has_tools = cJSON_GetObjectItem(resp, "has_tools");
    ck_assert_int_eq(has_tools->valueint, 1);
    cJSON *tc_arr = cJSON_GetObjectItem(resp, "tool_calls");
    ck_assert_ptr_nonnull(tc_arr);
    ck_assert_int_eq(cJSON_GetArraySize(tc_arr), 2);
    cJSON *tc0 = cJSON_GetArrayItem(tc_arr, 0);
    ck_assert_str_eq(cJSON_GetObjectItem(tc0, "name")->valuestring, "search");
    ck_assert_str_eq(cJSON_GetObjectItem(tc0, "arguments")->valuestring,
                     "{\"q\":\"test\"}");
    cJSON *tc1 = cJSON_GetArrayItem(tc_arr, 1);
    ck_assert_str_eq(cJSON_GetObjectItem(tc1, "name")->valuestring, "read_file");
    cJSON_Delete(resp);
}
END_TEST

START_TEST(test_chat_success_null_content)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    req.body = "{\"message\":\"hello\"}";
    req.body_len = 19;
    ctx.agent = (Agent *)&ctx;
    stub_agent_run_resp = &fake_resp_null_content;
    handle_chat(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    cJSON *resp = cJSON_Parse(captured_body);
    ck_assert_ptr_nonnull(resp);
    cJSON *content = cJSON_GetObjectItem(resp, "content");
    ck_assert_ptr_nonnull(content);
    ck_assert_str_eq(content->valuestring, "");
    cJSON_Delete(resp);
}
END_TEST

START_TEST(test_chat_success_tool_call_nulls)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    req.body = "{\"message\":\"run\"}";
    req.body_len = 17;
    ctx.agent = (Agent *)&ctx;
    stub_agent_run_resp = &fake_resp_tool_nulls;
    handle_chat(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    cJSON *resp = cJSON_Parse(captured_body);
    ck_assert_ptr_nonnull(resp);
    cJSON *tc_arr = cJSON_GetObjectItem(resp, "tool_calls");
    ck_assert_ptr_nonnull(tc_arr);
    cJSON *tc0 = cJSON_GetArrayItem(tc_arr, 0);
    ck_assert_str_eq(cJSON_GetObjectItem(tc0, "name")->valuestring, "");
    ck_assert_str_eq(cJSON_GetObjectItem(tc0, "arguments")->valuestring, "{}");
    cJSON_Delete(resp);
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_sse_stream auth tests
 * --------------------------------------------------------------------------- */

START_TEST(test_sse_no_token_returns_401)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.agent = (Agent *)&ctx;
    stub_unlock = 0;
    stub_query_token_match = NULL;
    handle_sse_stream(&req, (Client *)&ctx, &ctx);
    ck_assert_int_eq(captured_status, 401);
    ck_assert_str_eq(captured_body, "unauthorized");
}
END_TEST

START_TEST(test_sse_wrong_token_returns_401)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.agent = (Agent *)&ctx;
    stub_unlock = 0;
    strncpy(req.query, "token=wrong", sizeof(req.query) - 1);
    stub_query_token_match = NULL;
    handle_sse_stream(&req, (Client *)&ctx, &ctx);
    ck_assert_int_eq(captured_status, 401);
    ck_assert_str_eq(captured_body, "unauthorized");
}
END_TEST

START_TEST(test_sse_correct_token_query_opens_stream)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.agent = (Agent *)&ctx;
    stub_unlock = 0;
    strncpy(req.query, "token=secret", sizeof(req.query) - 1);
    stub_query_token_match = "secret";
    stub_agent_run_streaming_resp = &fake_resp_basic;
    stub_streaming_chunk_count = 1;
    stub_streaming_chunks[0] = "\"hello\"";
    handle_sse_stream(&req, (Client *)&ctx, &ctx);
    ck_assert_int_eq(captured_close_count, 1);
    ck_assert(captured_sse_count >= 2);
    ck_assert(strstr(captured_sse_buf, "\"type\":\"content\"") != NULL);
}
END_TEST

START_TEST(test_sse_token_with_trailing_ampersand)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.agent = (Agent *)&ctx;
    stub_unlock = 0;
    strncpy(req.query, "token=abc&foo=bar", sizeof(req.query) - 1);
    stub_query_token_match = "abc";
    stub_agent_run_streaming_resp = &fake_resp_basic;
    stub_streaming_chunk_count = 0;
    handle_sse_stream(&req, (Client *)&ctx, &ctx);
    ck_assert_int_eq(captured_close_count, 1);
    ck_assert(strstr(captured_sse_buf, "\"type\":\"done\"") != NULL);
}
END_TEST

START_TEST(test_sse_empty_token_returns_401)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.agent = (Agent *)&ctx;
    stub_unlock = 0;
    strncpy(req.query, "token=", sizeof(req.query) - 1);
    stub_query_token_match = NULL;
    handle_sse_stream(&req, (Client *)&ctx, &ctx);
    ck_assert_int_eq(captured_status, 401);
    ck_assert_str_eq(captured_body, "unauthorized");
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_sse_stream tests
 * --------------------------------------------------------------------------- */

START_TEST(test_sse_no_agent)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.agent = NULL;
    handle_sse_stream(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);
    ck_assert_str_eq(captured_body, "no agent");
}
END_TEST

START_TEST(test_sse_null_response)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.agent = (Agent *)&ctx;
    stub_agent_run_streaming_resp = NULL;
    handle_sse_stream(&req, (Client *)&ctx, &ctx);
    ck_assert_int_eq(captured_close_count, 1);
    ck_assert(captured_sse_count >= 2);
    ck_assert(strstr(captured_sse_buf, "\"type\":\"error\"") != NULL);
    ck_assert(strstr(captured_sse_buf, "no response") != NULL);
}
END_TEST

START_TEST(test_sse_success)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.agent = (Agent *)&ctx;
    stub_agent_run_streaming_resp = &fake_resp_basic;
    stub_streaming_chunk_count = 2;
    stub_streaming_chunks[0] = "Hello";
    stub_streaming_chunks[1] = " World\nwith \"quotes\"";
    handle_sse_stream(&req, (Client *)&ctx, &ctx);
    ck_assert_int_eq(captured_close_count, 1);
    ck_assert_int_eq(captured_sse_count, 4);
    ck_assert(strstr(captured_sse_buf, "\"type\":\"done\"") != NULL);
    ck_assert(strstr(captured_sse_buf, "\"type\":\"content\"") != NULL);
    ck_assert(strstr(captured_sse_buf, "\"content\":\"Hello\"") != NULL);
    ck_assert(strstr(captured_sse_buf, "World\\nwith \\\"quotes\\\"") != NULL);
}
END_TEST

START_TEST(test_sse_chunk_null_callback_data)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.agent = (Agent *)&ctx;
    stub_agent_run_streaming_resp = &fake_resp_basic;
    stub_streaming_chunk_count = 1;
    stub_streaming_chunks[0] = NULL;
    handle_sse_stream(&req, (Client *)&ctx, &ctx);
    ck_assert_int_eq(captured_close_count, 1);
    ck_assert(strstr(captured_sse_buf, "\"type\":\"done\"") != NULL);
}
END_TEST

START_TEST(test_sse_chunk_userdata_null_still_runs)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.agent = (Agent *)&ctx;
    stub_agent_run_streaming_resp = &fake_resp_basic;
    stub_streaming_chunk_count = 0;
    handle_sse_stream(&req, (Client *)&ctx, &ctx);
    ck_assert_int_eq(captured_close_count, 1);
    ck_assert(strstr(captured_sse_buf, "\"type\":\"done\"") != NULL);
}
END_TEST

START_TEST(test_sse_no_chunks)
{
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.agent = (Agent *)&ctx;
    stub_agent_run_streaming_resp = &fake_resp_basic;
    stub_streaming_chunk_count = 0;
    handle_sse_stream(&req, (Client *)&ctx, &ctx);
    ck_assert_int_eq(captured_close_count, 1);
    ck_assert(strstr(captured_sse_buf, "\"type\":\"done\"") != NULL);
    ck_assert(strstr(captured_sse_buf, "\"type\":\"content\"") == NULL);
}
END_TEST

START_TEST(test_sse_headers_oom_responds_500)
{
    /* First allocation in handle_sse_stream is the SSE headers asprintf;
     * on failure nothing has been written yet, so a 500 must be sent
     * instead of returning with the connection left dangling. */
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.agent = (Agent *)&ctx;
    routes_chat_test_set_alloc_fail(1);
    handle_sse_stream(&req, (Client *)&ctx, &ctx);
    routes_chat_test_set_alloc_fail(-1);
    ck_assert_int_eq(captured_status, 500);
}
END_TEST

START_TEST(test_sse_ctx_oom_writes_error_frame_and_closes)
{
    /* Second allocation is the SSECtx calloc, after the 200 SSE headers
     * are already on the wire; a 500 would be invalid now, so an SSE
     * error frame must be written and the client closed. */
    HTTPRequest req = {0};
    ServerContext ctx = {0};
    ctx.agent = (Agent *)&ctx;
    routes_chat_test_set_alloc_fail(2);
    handle_sse_stream(&req, (Client *)&ctx, &ctx);
    routes_chat_test_set_alloc_fail(-1);
    ck_assert_int_eq(captured_close_count, 1);
    ck_assert(strstr(captured_sse_buf, "\"type\":\"error\"") != NULL);
}
END_TEST

Suite *routes_chat_suite(void)
{
    Suite *s = suite_create("routes_chat");
    TCase *tc = tcase_create("handle_chat");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_chat_no_unlock);
    tcase_add_test(tc, test_chat_no_body);
    tcase_add_test(tc, test_chat_empty_body);
    tcase_add_test(tc, test_chat_invalid_json);
    tcase_add_test(tc, test_chat_missing_message);
    tcase_add_test(tc, test_chat_message_null);
    tcase_add_test(tc, test_chat_no_agent);
    tcase_add_test(tc, test_chat_agent_returns_null);
    tcase_add_test(tc, test_chat_returns_200_with_assistant_response);
    tcase_add_test(tc, test_chat_success_with_thinking);
    tcase_add_test(tc, test_chat_success_with_tool_calls);
    tcase_add_test(tc, test_chat_success_null_content);
    tcase_add_test(tc, test_chat_success_tool_call_nulls);
    suite_add_tcase(s, tc);

    TCase *tc_sse = tcase_create("handle_sse_stream");
    tcase_add_checked_fixture(tc_sse, setup, teardown);
    tcase_add_test(tc_sse, test_sse_no_token_returns_401);
    tcase_add_test(tc_sse, test_sse_wrong_token_returns_401);
    tcase_add_test(tc_sse, test_sse_correct_token_query_opens_stream);
    tcase_add_test(tc_sse, test_sse_token_with_trailing_ampersand);
    tcase_add_test(tc_sse, test_sse_empty_token_returns_401);
    tcase_add_test(tc_sse, test_sse_no_agent);
    tcase_add_test(tc_sse, test_sse_null_response);
    tcase_add_test(tc_sse, test_sse_success);
    tcase_add_test(tc_sse, test_sse_chunk_null_callback_data);
    tcase_add_test(tc_sse, test_sse_chunk_userdata_null_still_runs);
    tcase_add_test(tc_sse, test_sse_no_chunks);
    tcase_add_test(tc_sse, test_sse_headers_oom_responds_500);
    tcase_add_test(tc_sse, test_sse_ctx_oom_writes_error_frame_and_closes);
    suite_add_tcase(s, tc_sse);

    return s;
}

int main(void)
{
    Suite *s = routes_chat_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed;
}
