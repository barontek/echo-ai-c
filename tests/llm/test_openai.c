#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "llm/openai.h"
#include "llm/openai_oauth.h"
#include "utils/logging.h"

/* test_openai - unit tests for openai. Depends on: check, the module under test. */
struct OpenAIOAuth {
    int refresh_result;
};

static int refresh_calls = 0;
static int get_token_calls = 0;
static char rejected_token_seen[64] = {0};

void log_msg(LogLevel level, const char *file, int line,
             const char *message, ...)
{
    (void)level;
    (void)file;
    (void)line;
    (void)message;
}

static char *test_dup(const char *value)
{
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy) memcpy(copy, value, length);
    return copy;
}

static int json_allocation_count = 0;
static int json_allocation_fail_at = -1;

static void *test_json_malloc(size_t size)
{
    json_allocation_count++;
    if (json_allocation_count == json_allocation_fail_at) return NULL;
    return malloc(size);
}

static void test_json_free(void *allocation)
{
    free(allocation);
}

int openai_oauth_get_access_token(OpenAIOAuth *auth, char **access_token,
                                  char **account_id)
{
    get_token_calls++;
    (void)auth;
    if (access_token) *access_token = NULL;
    if (account_id) *account_id = NULL;
    return -1;
}

OpenAIOAuthTokenResult openai_oauth_refresh_after_401(
    OpenAIOAuth *auth, const char *rejected_token, char **access_token,
    char **account_id)
{
    refresh_calls++;
    int written = snprintf(rejected_token_seen, sizeof(rejected_token_seen),
                           "%s", rejected_token ? rejected_token : "");
    if (written < 0 || (size_t)written >= sizeof(rejected_token_seen) ||
        !auth || auth->refresh_result != 0)
        return OPENAI_OAUTH_TOKEN_TRANSIENT;
    *access_token = test_dup("replacement-token");
    *account_id = test_dup("account-new");
    if (!*access_token || !*account_id)
    {
        free(*access_token);
        free(*account_id);
        *access_token = NULL;
        *account_id = NULL;
        return OPENAI_OAUTH_TOKEN_TRANSIENT;
    }
    return OPENAI_OAUTH_TOKEN_OK;
}

static cJSON *parse_json_or_fail(const char *text)
{
    cJSON *json = cJSON_Parse(text);
    ck_assert_ptr_nonnull(json);
    return json;
}

START_TEST(test_request_converts_messages_tools_and_tool_outputs)
{
    ToolCall call = {.id = "call-1", .name = "lookup",
                     .arguments = "{\"q\":\"echo\"}"};
    Message messages[] = {
        {.role = "system", .content = "Be exact."},
        {.role = "developer", .content = "Use tools."},
        {.role = "user", .content = "Find echo."},
        {.role = "assistant", .content = "Checking.",
         .phase = "commentary",
         .tool_calls = &call, .tool_calls_count = 1},
        {.role = "tool", .content = "found", .tool_call_id = "call-1"}
    };
    const char *tools =
        "[{\"type\":\"function\",\"function\":{\"name\":\"lookup\","
        "\"description\":\"Look up a value\",\"parameters\":{\"type\":"
        "\"object\",\"properties\":{\"q\":{\"type\":\"string\"}}},"
        "\"strict\":true}}]";
    char *body = openai_test_build_request_body_alloc(messages, 5, "gpt-5-codex",
                                                 0.4, 1, tools, NULL, NULL);
    ck_assert_ptr_nonnull(body);
    cJSON *root = parse_json_or_fail(body);
    ck_assert_str_eq(cJSON_GetStringValue(cJSON_GetObjectItem(root, "model")),
                     "gpt-5-codex");
    ck_assert(cJSON_IsTrue(cJSON_GetObjectItem(root, "stream")));
    ck_assert(cJSON_IsFalse(cJSON_GetObjectItem(root, "store")));
    ck_assert_ptr_null(cJSON_GetObjectItem(root, "temperature"));
    cJSON *include = cJSON_GetObjectItem(root, "include");
    ck_assert_int_eq(cJSON_GetArraySize(include), 1);
    ck_assert_str_eq(cJSON_GetStringValue(cJSON_GetArrayItem(include, 0)),
                     "reasoning.encrypted_content");
    ck_assert_str_eq(cJSON_GetStringValue(cJSON_GetObjectItem(root, "instructions")),
                     "Be exact.\nUse tools.");

    cJSON *input = cJSON_GetObjectItem(root, "input");
    ck_assert_int_eq(cJSON_GetArraySize(input), 4);
    cJSON *user = cJSON_GetArrayItem(input, 0);
    cJSON *user_part = cJSON_GetArrayItem(cJSON_GetObjectItem(user, "content"), 0);
    ck_assert_str_eq(cJSON_GetStringValue(cJSON_GetObjectItem(user_part, "type")),
                     "input_text");
    cJSON *assistant = cJSON_GetArrayItem(input, 1);
    ck_assert_str_eq(cJSON_GetStringValue(cJSON_GetObjectItem(assistant, "phase")),
                     "commentary");
    cJSON *assistant_part = cJSON_GetArrayItem(
        cJSON_GetObjectItem(assistant, "content"), 0);
    ck_assert_str_eq(cJSON_GetStringValue(
                         cJSON_GetObjectItem(assistant_part, "type")),
                     "output_text");
    cJSON *function_call = cJSON_GetArrayItem(input, 2);
    ck_assert_str_eq(cJSON_GetStringValue(
                         cJSON_GetObjectItem(function_call, "call_id")),
                     "call-1");
    cJSON *tool_output = cJSON_GetArrayItem(input, 3);
    ck_assert_str_eq(cJSON_GetStringValue(cJSON_GetObjectItem(tool_output, "type")),
                     "function_call_output");
    ck_assert_str_eq(cJSON_GetStringValue(cJSON_GetObjectItem(tool_output, "output")),
                     "found");

    cJSON *converted_tools = cJSON_GetObjectItem(root, "tools");
    ck_assert_int_eq(cJSON_GetArraySize(converted_tools), 1);
    cJSON *converted = cJSON_GetArrayItem(converted_tools, 0);
    ck_assert_str_eq(cJSON_GetStringValue(cJSON_GetObjectItem(converted, "name")),
                     "lookup");
    ck_assert_ptr_null(cJSON_GetObjectItem(converted, "function"));
    ck_assert(cJSON_IsObject(cJSON_GetObjectItem(converted, "parameters")));
    ck_assert(cJSON_IsTrue(cJSON_GetObjectItem(converted, "strict")));
    ck_assert(cJSON_IsTrue(cJSON_GetObjectItem(root, "parallel_tool_calls")));
    cJSON_Delete(root);
    free(body);
}
END_TEST

START_TEST(test_request_allows_empty_tool_list)
{
    Message user = {.role = "user", .content = "hello"};
    char *body = openai_test_build_request_body_alloc(
        &user, 1, "gpt-5-codex", 0.7, 1, "[]", NULL, NULL);
    ck_assert_ptr_nonnull(body);
    cJSON *root = parse_json_or_fail(body);
    ck_assert_ptr_null(cJSON_GetObjectItem(root, "tools"));
    ck_assert_ptr_null(cJSON_GetObjectItem(root, "parallel_tool_calls"));
    cJSON_Delete(root);
    free(body);
}
END_TEST

START_TEST(test_request_replays_encrypted_reasoning_before_tool_calls)
{
    ToolCall call = {.id = "call-1", .name = "lookup", .arguments = "{}"};
    Message messages[] = {
        {.role = "assistant", .content = "", .tool_calls = &call,
         .tool_calls_count = 1,
         .provider_state = "[{\"type\":\"reasoning\",\"id\":\"r1\","
                           "\"encrypted_content\":\"ciphertext\"}]"},
        {.role = "tool", .content = "done", .tool_call_id = "call-1"}
    };
    char *body = openai_test_build_request_body_alloc(
        messages, 2, "gpt-5-codex", 0.7, 1, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(body);
    cJSON *root = parse_json_or_fail(body);
    cJSON *input = cJSON_GetObjectItem(root, "input");
    ck_assert_int_eq(cJSON_GetArraySize(input), 3);
    ck_assert_str_eq(cJSON_GetStringValue(cJSON_GetObjectItem(
                         cJSON_GetArrayItem(input, 0), "type")), "reasoning");
    ck_assert_str_eq(cJSON_GetStringValue(cJSON_GetObjectItem(
                         cJSON_GetArrayItem(input, 0), "encrypted_content")),
                     "ciphertext");
    cJSON_Delete(root);
    free(body);
}
END_TEST

START_TEST(test_request_rejects_invalid_roles_tools_and_tool_outputs)
{
    Message invalid_role = {.role = "moderator", .content = "no"};
    ck_assert_ptr_null(openai_test_build_request_body_alloc(
        &invalid_role, 1, "gpt-5-codex", 0.7, 0, NULL, NULL, NULL));

    Message orphan_output = {.role = "tool", .content = "result",
                             .tool_call_id = "missing"};
    ck_assert_ptr_null(openai_test_build_request_body_alloc(
        &orphan_output, 1, "gpt-5-codex", 0.7, 0, NULL, NULL, NULL));

    Message user = {.role = "user", .content = "hello"};
    ck_assert_ptr_null(openai_test_build_request_body_alloc(
        &user, 1, "gpt-5-codex", 0.7, 0,
        "[{\"type\":\"function\",\"function\":{\"name\":\"bad\","
        "\"parameters\":\"not-an-object\"}}]", NULL, NULL));
    ck_assert_ptr_null(openai_test_build_request_body_alloc(
        &user, 1, "gpt-5-codex", -0.1, 0, NULL, NULL, NULL));
}
END_TEST

START_TEST(test_request_translates_structured_output_schema)
{
    Message user = {.role = "user", .content = "extract"};
    const char *schema =
        "{\"type\":\"object\",\"properties\":{\"answer\":{\"type\":"
        "\"string\"}},\"required\":[\"answer\"],\"additionalProperties\":false}";
    char *body = openai_test_build_request_body_alloc(&user, 1, "gpt-5-codex",
                                                 0.2, 0, NULL, schema, NULL);
    ck_assert_ptr_nonnull(body);
    cJSON *root = parse_json_or_fail(body);
    cJSON *text = cJSON_GetObjectItem(root, "text");
    cJSON *format = cJSON_GetObjectItem(text, "format");
    ck_assert_str_eq(cJSON_GetStringValue(cJSON_GetObjectItem(format, "type")),
                     "json_schema");
    ck_assert_str_eq(cJSON_GetStringValue(cJSON_GetObjectItem(format, "name")),
                     "echo_structured_output");
    ck_assert(cJSON_IsTrue(cJSON_GetObjectItem(format, "strict")));
    ck_assert(cJSON_IsFalse(cJSON_GetObjectItem(
        cJSON_GetObjectItem(format, "schema"), "additionalProperties")));
    cJSON_Delete(root);
    free(body);
    ck_assert_ptr_null(openai_test_build_request_body_alloc(
        &user, 1, "gpt-5-codex", 0.2, 0, NULL, "[]", NULL));
}
END_TEST

START_TEST(test_request_cleans_up_each_cjson_allocation_failure)
{
    Message user = {.role = "user", .content = "extract"};
    cJSON_Hooks hooks = {.malloc_fn = test_json_malloc,
                         .free_fn = test_json_free};
    int succeeded = 0;
    for (int fail_at = 1; fail_at < 128; fail_at++)
    {
        json_allocation_count = 0;
        json_allocation_fail_at = fail_at;
        cJSON_InitHooks(&hooks);
        char *body = openai_test_build_request_body_alloc(
            &user, 1, "gpt-5-codex", 0.2, 0,
            "[{\"type\":\"function\",\"function\":{\"name\":\"lookup\","
            "\"parameters\":{\"type\":\"object\"}}}]",
            "{\"type\":\"object\"}", NULL);
        cJSON_InitHooks(NULL);
        if (body)
        {
            free(body);
            succeeded = 1;
            break;
        }
    }
    json_allocation_fail_at = -1;
    cJSON_InitHooks(NULL);
    ck_assert_int_eq(succeeded, 1);
}
END_TEST

START_TEST(test_buffered_response_reads_all_content_and_function_calls)
{
    const char *raw =
        "{\"status\":\"completed\",\"output\":["
        "{\"type\":\"reasoning\",\"summary\":[]},"
        "{\"type\":\"message\",\"content\":["
        "{\"type\":\"output_text\",\"text\":\"one \"},"
        "{\"type\":\"output_text\",\"text\":\"two\"}]},"
        "{\"type\":\"message\",\"content\":["
        "{\"type\":\"refusal\",\"refusal\":\" blocked\"}]},"
        "{\"type\":\"function_call\",\"call_id\":\"c1\","
        "\"name\":\"first\",\"arguments\":\"{}\"},"
        "{\"type\":\"function_call\",\"call_id\":\"c2\","
        "\"name\":\"second\",\"arguments\":\"{\\\"x\\\":1}\"}]}";
    LLMResponse *response = openai_test_parse_response_alloc(raw);
    ck_assert_ptr_nonnull(response);
    ck_assert_str_eq(response->content, "one two blocked");
    ck_assert_int_eq(response->tool_calls_count, 2);
    ck_assert_str_eq(response->tool_calls[0].id, "c1");
    ck_assert_str_eq(response->tool_calls[0].name, "first");
    ck_assert_str_eq(response->tool_calls[1].id, "c2");
    ck_assert_str_eq(response->tool_calls[1].arguments, "{\"x\":1}");
    llm_response_free(response);
}
END_TEST

START_TEST(test_buffered_response_extracts_reasoning_summary_as_thinking)
{
    const char *raw =
        "{\"status\":\"completed\",\"output\":["
        "{\"type\":\"reasoning\",\"summary\":["
        "{\"type\":\"summary_text\",\"text\":\"Let me think\"}]},"
        "{\"type\":\"message\",\"content\":["
        "{\"type\":\"output_text\",\"text\":\"Answer\"}]}]}";
    LLMResponse *response = openai_test_parse_response_alloc(raw);
    ck_assert_ptr_nonnull(response);
    ck_assert_str_eq(response->thinking, "Let me think");
    ck_assert_str_eq(response->content,
                     "<think>\nLet me think\n</think>\n\nAnswer");
    llm_response_free(response);
}
END_TEST

START_TEST(test_buffered_response_without_summary_keeps_plain_content)
{
    const char *raw =
        "{\"status\":\"completed\",\"output\":["
        "{\"type\":\"message\",\"content\":["
        "{\"type\":\"output_text\",\"text\":\"plain\"}]}]}";
    LLMResponse *response = openai_test_parse_response_alloc(raw);
    ck_assert_ptr_nonnull(response);
    ck_assert_ptr_null(response->thinking);
    ck_assert_str_eq(response->content, "plain");
    llm_response_free(response);
}
END_TEST

START_TEST(test_buffered_response_rejects_errors_and_malformed_calls)
{
    ck_assert_ptr_null(openai_test_parse_response_alloc(
        "{\"status\":\"failed\",\"error\":{\"code\":\"bad\"},\"output\":[]}"));
    ck_assert_ptr_null(openai_test_parse_response_alloc(
        "{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\","
        "\"name\":\"missing_id\",\"arguments\":\"{}\"}]}"));
    ck_assert_ptr_null(openai_test_parse_response_alloc(
        "{\"status\":\"completed\",\"output\":{}}"));
}
END_TEST

START_TEST(test_models_catalog_keeps_visible_unique_slugs_in_server_order)
{
    const char *raw =
        "{\"models\":["
        "{\"slug\":\"gpt-5.4\",\"visibility\":\"list\",\"priority\":1},"
        "{\"slug\":\"internal-model\",\"visibility\":\"hide\"},"
        "{\"slug\":\"gpt-5.3-codex\",\"visibility\":\"list\"},"
        "{\"slug\":\"gpt-5.4\",\"visibility\":\"list\"}]}";
    char **models = NULL;
    size_t count = 0U;
    ck_assert_int_eq(openai_test_parse_models(raw, &models, &count), 0);
    ck_assert_uint_eq(count, 2U);
    ck_assert_str_eq(models[0], "gpt-5.4");
    ck_assert_str_eq(models[1], "gpt-5.3-codex");
    openai_models_free(models, count);

    models = (char **)raw;
    count = 9U;
    ck_assert_int_eq(openai_test_parse_models(
        "{\"models\":[{\"slug\":\"broken\"}]}", &models, &count), 0);
    ck_assert_ptr_null(models);
    ck_assert_uint_eq(count, 0U);
}
END_TEST

START_TEST(test_models_catalog_skips_malformed_entries)
{
    const char *raw =
        "{\"models\":["
        "{\"slug\":\"gpt-5.4\",\"visibility\":\"list\",\"priority\":1},"
        "{\"slug\":\"broken-no-visibility\"},"
        "\"not-an-object\","
        "{\"visibility\":\"list\",\"priority\":2},"
        "{\"slug\":\"gpt-5.3-codex\",\"visibility\":\"list\"}]}";
    char **models = NULL;
    size_t count = 0U;
    ck_assert_int_eq(openai_test_parse_models(raw, &models, &count), 0);
    ck_assert_uint_eq(count, 2U);
    ck_assert_str_eq(models[0], "gpt-5.4");
    ck_assert_str_eq(models[1], "gpt-5.3-codex");
    openai_models_free(models, count);

    char **bad_top = (char **)raw;
    size_t bad_count = 9U;
    ck_assert_int_eq(openai_test_parse_models(
        "{\"models\":{\"slug\":\"gpt-5.4\"}}", &bad_top, &bad_count), -1);
    ck_assert_ptr_null(bad_top);
    ck_assert_uint_eq(bad_count, 0U);
}


START_TEST(test_request_metadata_uses_codex_endpoint_headers_and_timeout)
{
    char *url = NULL;
    char *headers = NULL;
    long timeout = 0L;
    ck_assert_int_eq(openai_test_request_metadata(
        "oauth-token", "acct-1", "{}", 37, &url, &headers, &timeout), 0);
    ck_assert_str_eq(url, "https://chatgpt.com/backend-api/codex/responses");
    ck_assert_int_eq(timeout, 37L);
    ck_assert_ptr_nonnull(strstr(headers, "Authorization: Bearer oauth-token\n"));
    ck_assert_ptr_nonnull(strstr(headers, "ChatGPT-Account-Id: acct-1\n"));
    ck_assert_ptr_nonnull(strstr(headers, "originator: echo-ai\n"));
    free(url);
    free(headers);
    ck_assert_int_eq(openai_test_request_metadata(
        "oauth-token", "bad\r\nheader", "{}", 37,
        &url, &headers, &timeout), -1);
    ck_assert_int_eq(openai_test_request_metadata(
        "oauth-token", NULL, "{}", 0, &url, &headers, &timeout), -1);
}
END_TEST

START_TEST(test_unauthorized_refresh_uses_rejected_token_and_new_credentials)
{
    OpenAIOAuth auth = {.refresh_result = 0};
    char *token = NULL;
    char *account = NULL;
    refresh_calls = 0;
    rejected_token_seen[0] = '\0';
    ck_assert_int_eq(openai_test_refresh_after_401(
        &auth, "rejected-token", &token, &account), 0);
    ck_assert_int_eq(refresh_calls, 1);
    ck_assert_str_eq(rejected_token_seen, "rejected-token");
    ck_assert_str_eq(token, "replacement-token");
    ck_assert_str_eq(account, "account-new");
    free(token);
    free(account);
}
END_TEST

START_TEST(test_unauthorized_refresh_failure_returns_error)
{
    OpenAIOAuth auth = {.refresh_result = -1};
    char *token = NULL;
    char *account = NULL;
    ck_assert_int_eq(openai_test_refresh_after_401(
        &auth, "rejected-token", &token, &account), -1);
    ck_assert_ptr_null(token);
    ck_assert_ptr_null(account);
}
END_TEST

START_TEST(test_provider_ignores_static_configuration_and_requires_oauth_for_requests)
{
    OpenAIOAuth auth = {0};
    LLMProvider *signed_out = openai_provider_create(
        "https://api.openai.com/v1", "static-api-key", NULL, NULL);
    ck_assert_ptr_null(signed_out);
    LLMProvider *provider = openai_provider_create(
        "https://api.openai.com/v1", "static-api-key", NULL, &auth);
    ck_assert_ptr_nonnull(provider);
    ck_assert(provider->chat != NULL);
    ck_assert(provider->chat_streaming != NULL);
    ck_assert(provider->extract_structured != NULL);
    provider->destroy(provider);
}
END_TEST

START_TEST(test_provider_accepts_valid_effort_values)
{
    OpenAIOAuth auth = {0};
    const char *valid[] = {"low", "medium", "high", "xhigh", "max", "none"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
    {
        LLMProvider *provider = openai_provider_create(
            NULL, NULL, valid[i], &auth);
        ck_assert_ptr_nonnull(provider);
        provider->destroy(provider);
    }
}
END_TEST

START_TEST(test_provider_rejects_invalid_effort)
{
    OpenAIOAuth auth = {0};
    LLMProvider *provider = openai_provider_create(
        NULL, NULL, "extreme", &auth);
    ck_assert_ptr_null(provider);
    provider = openai_provider_create(
        NULL, NULL, "high-effort", &auth);
    ck_assert_ptr_null(provider);
    /* "minimal" was dropped from the accepted set. */
    provider = openai_provider_create(
        NULL, NULL, "minimal", &auth);
    ck_assert_ptr_null(provider);
}
END_TEST

START_TEST(test_request_sends_reasoning_effort_when_configured)
{
    Message user = {.role = "user", .content = "hello"};
    char *body = openai_test_build_request_body_alloc(
        &user, 1, "gpt-5-codex", 0.7, 1, NULL, NULL, "high");
    ck_assert_ptr_nonnull(body);
    cJSON *root = parse_json_or_fail(body);
    cJSON *reasoning = cJSON_GetObjectItem(root, "reasoning");
    ck_assert(cJSON_IsObject(reasoning));
    ck_assert_str_eq(cJSON_GetStringValue(
                         cJSON_GetObjectItem(reasoning, "effort")), "high");
    /* summaries are requested even alongside the effort hint */
    ck_assert_str_eq(cJSON_GetStringValue(
                         cJSON_GetObjectItem(reasoning, "summary")), "auto");
    cJSON_Delete(root);
    free(body);
}
END_TEST

START_TEST(test_request_requests_summary_when_effort_unset)
{
    Message user = {.role = "user", .content = "hello"};
    char *body = openai_test_build_request_body_alloc(
        &user, 1, "gpt-5-codex", 0.7, 1, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(body);
    cJSON *root = parse_json_or_fail(body);
    cJSON *reasoning = cJSON_GetObjectItem(root, "reasoning");
    ck_assert(cJSON_IsObject(reasoning));
    ck_assert_str_eq(cJSON_GetStringValue(
                         cJSON_GetObjectItem(reasoning, "summary")), "auto");
    ck_assert_ptr_null(cJSON_GetObjectItem(reasoning, "effort"));
    cJSON_Delete(root);
    free(body);
}
END_TEST

START_TEST(test_request_rejects_invalid_effort)
{
    Message user = {.role = "user", .content = "hello"};
    ck_assert_ptr_null(openai_test_build_request_body_alloc(
        &user, 1, "gpt-5-codex", 0.7, 1, NULL, NULL, "extreme"));
    ck_assert_ptr_null(openai_test_build_request_body_alloc(
        &user, 1, "gpt-5-codex", 0.7, 1, NULL, NULL, "MEDIUM"));
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("OpenAI Codex");
    TCase *request = tcase_create("Request Conversion");
    tcase_add_test(request, test_request_converts_messages_tools_and_tool_outputs);
    tcase_add_test(request, test_request_rejects_invalid_roles_tools_and_tool_outputs);
    tcase_add_test(request, test_request_translates_structured_output_schema);
    tcase_add_test(request, test_request_allows_empty_tool_list);
    tcase_add_test(request,
                   test_request_replays_encrypted_reasoning_before_tool_calls);
    tcase_add_test(request, test_request_sends_reasoning_effort_when_configured);
    tcase_add_test(request, test_request_requests_summary_when_effort_unset);
    tcase_add_test(request, test_request_rejects_invalid_effort);
    tcase_add_test(request, test_request_cleans_up_each_cjson_allocation_failure);
    suite_add_tcase(suite, request);

    TCase *buffered = tcase_create("Buffered Responses");
    tcase_add_test(buffered, test_buffered_response_reads_all_content_and_function_calls);
    tcase_add_test(buffered, test_buffered_response_extracts_reasoning_summary_as_thinking);
    tcase_add_test(buffered, test_buffered_response_without_summary_keeps_plain_content);
    tcase_add_test(buffered, test_buffered_response_rejects_errors_and_malformed_calls);
    tcase_add_test(buffered,
                   test_models_catalog_keeps_visible_unique_slugs_in_server_order);
    tcase_add_test(buffered, test_models_catalog_skips_malformed_entries);
    suite_add_tcase(suite, buffered);


    TCase *transport = tcase_create("Transport Contract");
    tcase_set_timeout(transport, 5);
    tcase_add_test(transport, test_request_metadata_uses_codex_endpoint_headers_and_timeout);
    tcase_add_test(transport, test_unauthorized_refresh_uses_rejected_token_and_new_credentials);
    tcase_add_test(transport, test_unauthorized_refresh_failure_returns_error);
    tcase_add_test(transport, test_provider_ignores_static_configuration_and_requires_oauth_for_requests);
    tcase_add_test(transport, test_provider_accepts_valid_effort_values);
    tcase_add_test(transport, test_provider_rejects_invalid_effort);
    suite_add_tcase(suite, transport);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed == 0 ? 0 : 1;
}
