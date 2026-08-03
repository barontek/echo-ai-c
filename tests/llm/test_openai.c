#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "llm/openai.h"
#include "llm/openai_oauth.h"
#include "utils/logging.h"

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
    char *body = openai_test_build_request_body(messages, 5, "gpt-5-codex",
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
    char *body = openai_test_build_request_body(
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
    char *body = openai_test_build_request_body(
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
    ck_assert_ptr_null(openai_test_build_request_body(
        &invalid_role, 1, "gpt-5-codex", 0.7, 0, NULL, NULL, NULL));

    Message orphan_output = {.role = "tool", .content = "result",
                             .tool_call_id = "missing"};
    ck_assert_ptr_null(openai_test_build_request_body(
        &orphan_output, 1, "gpt-5-codex", 0.7, 0, NULL, NULL, NULL));

    Message user = {.role = "user", .content = "hello"};
    ck_assert_ptr_null(openai_test_build_request_body(
        &user, 1, "gpt-5-codex", 0.7, 0,
        "[{\"type\":\"function\",\"function\":{\"name\":\"bad\","
        "\"parameters\":\"not-an-object\"}}]", NULL, NULL));
    ck_assert_ptr_null(openai_test_build_request_body(
        &user, 1, "gpt-5-codex", -0.1, 0, NULL, NULL, NULL));
}
END_TEST

START_TEST(test_request_translates_structured_output_schema)
{
    Message user = {.role = "user", .content = "extract"};
    const char *schema =
        "{\"type\":\"object\",\"properties\":{\"answer\":{\"type\":"
        "\"string\"}},\"required\":[\"answer\"],\"additionalProperties\":false}";
    char *body = openai_test_build_request_body(&user, 1, "gpt-5-codex",
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
    ck_assert_ptr_null(openai_test_build_request_body(
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
        char *body = openai_test_build_request_body(
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
    LLMResponse *response = openai_test_parse_response(raw);
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

START_TEST(test_buffered_response_rejects_errors_and_malformed_calls)
{
    ck_assert_ptr_null(openai_test_parse_response(
        "{\"status\":\"failed\",\"error\":{\"code\":\"bad\"},\"output\":[]}"));
    ck_assert_ptr_null(openai_test_parse_response(
        "{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\","
        "\"name\":\"missing_id\",\"arguments\":\"{}\"}]}"));
    ck_assert_ptr_null(openai_test_parse_response(
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
END_TEST

typedef struct {
    char text[64];
    size_t length;
    int calls;
} ChunkCapture;

static void capture_chunk(const char *chunk, void *userdata)
{
    ChunkCapture *capture = userdata;
    size_t length = strlen(chunk);
    ck_assert(length <= sizeof(capture->text) - capture->length - 1U);
    memcpy(capture->text + capture->length, chunk, length + 1U);
    capture->length += length;
    capture->calls++;
}

START_TEST(test_fragmented_stream_maps_interleaved_function_calls)
{
    const char *stream =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hi \"}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":1,"
        "\"item\":{\"type\":\"function_call\",\"id\":\"item-a\","
        "\"call_id\":\"call-a\",\"name\":\"alpha\",\"arguments\":\"\"}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":2,"
        "\"item\":{\"type\":\"function_call\",\"id\":\"item-b\","
        "\"call_id\":\"call-b\",\"name\":\"beta\",\"arguments\":\"\"}}\n\n"
        "data: {\"type\":\"response.function_call_arguments.delta\","
        "\"output_index\":2,\"item_id\":\"item-b\",\"delta\":\"{\"}\n\n"
        "data: {\"type\":\"response.function_call_arguments.delta\","
        "\"output_index\":1,\"item_id\":\"item-a\",\"delta\":\"{\\\"x\\\":\"}\n\n"
        "data: {\"type\":\"response.function_call_arguments.done\","
        "\"output_index\":2,\"item_id\":\"item-b\",\"arguments\":\"{}\"}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":1,"
        "\"item\":{\"type\":\"function_call\",\"id\":\"item-a\","
        "\"call_id\":\"call-a\",\"name\":\"alpha\","
        "\"arguments\":\"{\\\"x\\\":1}\"}}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"there\"}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":3,"
        "\"item\":{\"type\":\"message\",\"role\":\"assistant\","
        "\"phase\":\"final_answer\",\"content\":[]}}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,"
        "\"item\":{\"type\":\"reasoning\",\"id\":\"reason-1\","
        "\"encrypted_content\":\"ciphertext\"}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"status\":\"completed\"}}";
    size_t total = strlen(stream);
    size_t lengths[] = {17U, 41U, 3U, 89U, total - 150U};
    const char *fragments[] = {stream, stream + 17U, stream + 58U,
                               stream + 61U, stream + 150U};
    ChunkCapture capture = {0};
    LLMResponse *response = openai_test_stream_fragments(
        fragments, lengths, 5, capture_chunk, &capture);
    ck_assert_ptr_nonnull(response);
    ck_assert_str_eq(response->content, "Hi there");
    ck_assert_str_eq(capture.text, "Hi there");
    ck_assert_int_eq(capture.calls, 2);
    ck_assert_int_eq(response->tool_calls_count, 2);
    ck_assert_str_eq(response->tool_calls[0].id, "call-a");
    ck_assert_str_eq(response->tool_calls[0].name, "alpha");
    ck_assert_str_eq(response->tool_calls[0].arguments, "{\"x\":1}");
    ck_assert_str_eq(response->tool_calls[1].id, "call-b");
    ck_assert_str_eq(response->tool_calls[1].name, "beta");
    ck_assert_str_eq(response->tool_calls[1].arguments, "{}");
    ck_assert_ptr_nonnull(response->provider_state);
    ck_assert_ptr_nonnull(strstr(response->provider_state, "ciphertext"));
    ck_assert_str_eq(response->phase, "final_answer");
    llm_response_free(response);
}
END_TEST

START_TEST(test_stream_requires_successful_terminal_event)
{
    const char *failed =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"partial\"}\n\n"
        "data: {\"type\":\"response.failed\",\"response\":{\"status\":"
        "\"failed\",\"error\":{\"code\":\"server_error\"}}}\n\n";
    const char *incomplete =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"partial\"}\n\n";
    ck_assert_ptr_null(openai_test_stream_fragments(&failed, NULL, 1, NULL, NULL));
    ck_assert_ptr_null(openai_test_stream_fragments(&incomplete, NULL, 1, NULL, NULL));
}
END_TEST

START_TEST(test_stream_accepts_fragmented_done_without_final_newline)
{
    const char *fragments[] = {
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"o",
        "k\"}\n\ndata: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":[]}}\n\ndata: [DO",
        "NE]"};
    LLMResponse *response = openai_test_stream_fragments(
        fragments, NULL, 3, NULL, NULL);
    ck_assert_ptr_nonnull(response);
    ck_assert_str_eq(response->content, "ok");
    llm_response_free(response);
}
END_TEST

START_TEST(test_stream_rejects_done_without_completed_and_returns_refusals)
{
    const char *done_only =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"partial\"}\n\n"
        "data: [DONE]\n\n";
    const char *refusal =
        "data: {\"type\":\"response.refusal.delta\",\"delta\":\"no\"}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"status\":\"completed\",\"output\":[]}}\n\n";
    ck_assert_ptr_null(openai_test_stream_fragments(
        &done_only, NULL, 1, NULL, NULL));
    LLMResponse *response = openai_test_stream_fragments(
        &refusal, NULL, 1, NULL, NULL);
    ck_assert_ptr_nonnull(response);
    ck_assert_str_eq(response->content, "no");
    llm_response_free(response);
}
END_TEST

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

    auth.refresh_result = -1;
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
    char *body = openai_test_build_request_body(
        &user, 1, "gpt-5-codex", 0.7, 1, NULL, NULL, "high");
    ck_assert_ptr_nonnull(body);
    cJSON *root = parse_json_or_fail(body);
    cJSON *reasoning = cJSON_GetObjectItem(root, "reasoning");
    ck_assert(cJSON_IsObject(reasoning));
    ck_assert_str_eq(cJSON_GetStringValue(
                         cJSON_GetObjectItem(reasoning, "effort")), "high");
    cJSON_Delete(root);
    free(body);
}
END_TEST

START_TEST(test_request_omits_reasoning_when_effort_unset)
{
    Message user = {.role = "user", .content = "hello"};
    char *body = openai_test_build_request_body(
        &user, 1, "gpt-5-codex", 0.7, 1, NULL, NULL, NULL);
    ck_assert_ptr_nonnull(body);
    cJSON *root = parse_json_or_fail(body);
    ck_assert_ptr_null(cJSON_GetObjectItem(root, "reasoning"));
    cJSON_Delete(root);
    free(body);
}
END_TEST

START_TEST(test_request_rejects_invalid_effort)
{
    Message user = {.role = "user", .content = "hello"};
    ck_assert_ptr_null(openai_test_build_request_body(
        &user, 1, "gpt-5-codex", 0.7, 1, NULL, NULL, "extreme"));
    ck_assert_ptr_null(openai_test_build_request_body(
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
    tcase_add_test(request, test_request_omits_reasoning_when_effort_unset);
    tcase_add_test(request, test_request_rejects_invalid_effort);
    tcase_add_test(request, test_request_cleans_up_each_cjson_allocation_failure);
    suite_add_tcase(suite, request);

    TCase *buffered = tcase_create("Buffered Responses");
    tcase_add_test(buffered, test_buffered_response_reads_all_content_and_function_calls);
    tcase_add_test(buffered, test_buffered_response_rejects_errors_and_malformed_calls);
    tcase_add_test(buffered,
                   test_models_catalog_keeps_visible_unique_slugs_in_server_order);
    tcase_add_test(buffered, test_models_catalog_skips_malformed_entries);
    suite_add_tcase(suite, buffered);

    TCase *stream = tcase_create("Streaming Responses");
    tcase_set_timeout(stream, 5);
    tcase_add_test(stream, test_fragmented_stream_maps_interleaved_function_calls);
    tcase_add_test(stream, test_stream_requires_successful_terminal_event);
    tcase_add_test(stream, test_stream_accepts_fragmented_done_without_final_newline);
    tcase_add_test(stream,
                   test_stream_rejects_done_without_completed_and_returns_refusals);
    suite_add_tcase(suite, stream);

    TCase *transport = tcase_create("Transport Contract");
    tcase_set_timeout(transport, 5);
    tcase_add_test(transport, test_request_metadata_uses_codex_endpoint_headers_and_timeout);
    tcase_add_test(transport, test_unauthorized_refresh_uses_rejected_token_and_new_credentials);
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
