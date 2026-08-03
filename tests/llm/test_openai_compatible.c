#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "agent/message.h"

LLMResponse *openai_compatible_test_parse_stream(
    const char *input, void (*on_chunk)(const char *, void *), void *userdata);

LLMResponse *openai_compatible_test_stream_fragments(
    const char **fragments, size_t *lengths, int count,
    void (*on_chunk)(const char *, void *), void *userdata);

char *openai_compatible_test_build_url(const char *base_url);

char *openai_compatible_test_build_body(const char *model, const char *msgs_json,
                                        int stream, double temperature,
                                        const char *tools_json,
                                        const char *json_schema,
                                        int force_json_format,
                                        const char *effort);

typedef struct {
    char content[64];
    size_t len;
    int calls;
} ChunkCapture;

static void capture_chunk(const char *chunk, void *userdata)
{
    ChunkCapture *capture = userdata;
    size_t chunk_len = strlen(chunk);
    ck_assert(chunk_len <= sizeof(capture->content) - capture->len - 1);
    memcpy(capture->content + capture->len, chunk, chunk_len + 1);
    capture->len += chunk_len;
    capture->calls++;
}

START_TEST(test_openai_compatible_stream_accumulates_content_and_tool_calls)
{
    const char *stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"lo\",\"tool_calls\":[{\"index\":0,\"id\":\"call-1\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"} \"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    ChunkCapture capture = {0};
    LLMResponse *resp = openai_compatible_test_parse_stream(stream, capture_chunk, &capture);
    ck_assert_ptr_nonnull(resp);
    ck_assert_str_eq(resp->content, "Hello");
    ck_assert_int_eq(capture.calls, 2);
    ck_assert_str_eq(capture.content, "Hello");
    ck_assert_int_eq(resp->tool_calls_count, 1);
    ck_assert_str_eq(resp->tool_calls[0].id, "call-1");
    ck_assert_str_eq(resp->tool_calls[0].name, "read_file");
    ck_assert_str_eq(resp->tool_calls[0].arguments, "{} ");
    llm_response_free(resp);
}
END_TEST

START_TEST(test_openai_compatible_stream_handles_empty_response)
{
    LLMResponse *resp = openai_compatible_test_parse_stream("data: [DONE]", NULL, NULL);
    ck_assert_ptr_nonnull(resp);
    ck_assert_str_eq(resp->content, "");
    llm_response_free(resp);
}
END_TEST

START_TEST(test_openai_compatible_stream_splits_lines_across_fragments)
{
    /* Simulates TCP segments arriving mid-line: SSE lines split across
     * arbitrary write boundaries must still parse and emit chunks. */
    const char *frag1 = "data: {\"choices\":[{\"delta\":{\"content\":\"Hel";
    const char *frag2 = "lo\"}}]}]}\n\ndata: {\"choices\":[{\"delta\":{\"content\":\"!\"}}";
    const char *frag3 = "]}]}\n\n";
    const char *frag4 = "data: [DONE]\n\n";
    const char *fragments[] = {frag1, frag2, frag3, frag4};

    ChunkCapture capture = {0};
    LLMResponse *resp = openai_compatible_test_stream_fragments(
        fragments, NULL, 4, capture_chunk, &capture);
    ck_assert_ptr_nonnull(resp);
    ck_assert_str_eq(resp->content, "Hello!");
    ck_assert_int_eq(capture.calls, 2);
    ck_assert_str_eq(capture.content, "Hello!");
    llm_response_free(resp);
}
END_TEST

START_TEST(test_openai_compatible_stream_final_line_without_newline)
{
    /* Some servers omit the trailing newline of the last SSE line. */
    const char *frag1 = "data: {\"choices\":[{\"delta\":{\"content\":\"bye\"}}]}]}\n\n";
    const char *frag2 = "data: {\"choices\":[{\"delta\":{\"content\":\"!!\"}}]}]}";
    const char *fragments[] = {frag1, frag2};

    ChunkCapture capture = {0};
    LLMResponse *resp = openai_compatible_test_stream_fragments(
        fragments, NULL, 2, capture_chunk, &capture);
    ck_assert_ptr_nonnull(resp);
    ck_assert_str_eq(resp->content, "bye!!");
    ck_assert_int_eq(capture.calls, 2);
    llm_response_free(resp);
}
END_TEST

START_TEST(test_build_url_appends_v1_chat_completions)
{
    /* Plain endpoint without a version prefix gets /v1/chat/completions
     * appended (api.openai.com convention). */
    char *url = openai_compatible_test_build_url("https://api.openai.com");
    ck_assert_ptr_nonnull(url);
    ck_assert_str_eq(url, "https://api.openai.com/v1/chat/completions");
    free(url);
}
END_TEST

START_TEST(test_build_url_v1_base_appends_chat_completions)
{
    /* OpenCode Zen's documented base already includes /v1; appending
     * /v1/chat/completions would produce a doubled /v1/v1 path. */
    char *url = openai_compatible_test_build_url("https://opencode.ai/zen/v1");
    ck_assert_ptr_nonnull(url);
    ck_assert_str_eq(url, "https://opencode.ai/zen/v1/chat/completions");
    free(url);
}
END_TEST

START_TEST(test_build_url_bare_base_appends_v1_chat_completions)
{
    /* Host root without a version prefix gets the full /v1 path. */
    char *url = openai_compatible_test_build_url("https://opencode.ai/zen");
    ck_assert_ptr_nonnull(url);
    ck_assert_str_eq(url, "https://opencode.ai/zen/v1/chat/completions");
    free(url);
}
END_TEST

START_TEST(test_build_url_keeps_exact_chat_completions_endpoint)
{
    char *url = openai_compatible_test_build_url("https://opencode.ai/zen/v1/chat/completions");
    ck_assert_ptr_nonnull(url);
    ck_assert_str_eq(url, "https://opencode.ai/zen/v1/chat/completions");
    free(url);
}
END_TEST

START_TEST(test_body_embeds_reasoning_effort_when_set)
{
    char *body = openai_compatible_test_build_body(
        "qwen3", "[{\"role\":\"user\",\"content\":\"hi\"}]", 1, 0.7,
        NULL, NULL, 0, "none");
    ck_assert_ptr_nonnull(body);
    ck_assert(strstr(body, "\"reasoning_effort\":\"none\"") != NULL);
    ck_assert(strstr(body, "\"stream\":true") != NULL);
    free(body);

    body = openai_compatible_test_build_body(
        "qwen3", "[]", 0, 0.7, NULL, NULL, 0, "max");
    ck_assert_ptr_nonnull(body);
    ck_assert(strstr(body, "\"reasoning_effort\":\"max\"") != NULL);
    free(body);
}
END_TEST

START_TEST(test_body_omits_reasoning_effort_when_unset)
{
    char *body = openai_compatible_test_build_body(
        "qwen3", "[]", 0, 0.7, NULL, NULL, 0, NULL);
    ck_assert_ptr_nonnull(body);
    ck_assert_ptr_null(strstr(body, "reasoning_effort"));
    free(body);
}
END_TEST

START_TEST(test_body_rejects_invalid_effort)
{
    ck_assert_ptr_null(openai_compatible_test_build_body(
        "qwen3", "[]", 0, 0.7, NULL, NULL, 0, "xhigh"));
    ck_assert_ptr_null(openai_compatible_test_build_body(
        "qwen3", "[]", 0, 0.7, NULL, NULL, 0, "minimal"));
    ck_assert_ptr_null(openai_compatible_test_build_body(
        "qwen3", "[]", 0, 0.7, NULL, NULL, 0, "extreme"));
}
END_TEST

START_TEST(test_body_with_tools_and_schema_still_carries_effort)
{
    char *body = openai_compatible_test_build_body(
        "qwen3", "[]", 0, 0.7, "[{\"type\":\"function\"}]", NULL, 0, "high");
    ck_assert_ptr_nonnull(body);
    ck_assert(strstr(body, "\"reasoning_effort\":\"high\"") != NULL);
    ck_assert(strstr(body, "\"tools\":") != NULL);
    free(body);

    body = openai_compatible_test_build_body(
        "qwen3", "[]", 0, 0.7, NULL, "{\"type\":\"object\"}", 1, "low");
    ck_assert_ptr_nonnull(body);
    ck_assert(strstr(body, "\"reasoning_effort\":\"low\"") != NULL);
    ck_assert(strstr(body, "\"response_format\"") != NULL);
    free(body);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("OpenAI Compatible");
    TCase *tc = tcase_create("Streaming");
    tcase_add_test(tc, test_openai_compatible_stream_accumulates_content_and_tool_calls);
    tcase_add_test(tc, test_openai_compatible_stream_handles_empty_response);
    tcase_add_test(tc, test_openai_compatible_stream_splits_lines_across_fragments);
    tcase_add_test(tc, test_openai_compatible_stream_final_line_without_newline);
    suite_add_tcase(suite, tc);
    TCase *tc_url = tcase_create("BuildUrl");
    tcase_add_test(tc_url, test_build_url_appends_v1_chat_completions);
    tcase_add_test(tc_url, test_build_url_v1_base_appends_chat_completions);
    tcase_add_test(tc_url, test_build_url_bare_base_appends_v1_chat_completions);
    tcase_add_test(tc_url, test_build_url_keeps_exact_chat_completions_endpoint);
    suite_add_tcase(suite, tc_url);
    TCase *tc_body = tcase_create("RequestBody");
    tcase_add_test(tc_body, test_body_embeds_reasoning_effort_when_set);
    tcase_add_test(tc_body, test_body_omits_reasoning_effort_when_unset);
    tcase_add_test(tc_body, test_body_rejects_invalid_effort);
    tcase_add_test(tc_body, test_body_with_tools_and_schema_still_carries_effort);
    suite_add_tcase(suite, tc_body);
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
