#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "agent/message.h"

LLMResponse *lmstudio_test_parse_stream(
    const char *input, void (*on_chunk)(const char *, void *), void *userdata);

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

START_TEST(test_lmstudio_stream_accumulates_content_and_tool_calls)
{
    const char *stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"lo\",\"tool_calls\":[{\"index\":0,\"id\":\"call-1\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"} \"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    ChunkCapture capture = {0};
    LLMResponse *resp = lmstudio_test_parse_stream(stream, capture_chunk, &capture);
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

START_TEST(test_lmstudio_stream_handles_empty_response)
{
    LLMResponse *resp = lmstudio_test_parse_stream("data: [DONE]", NULL, NULL);
    ck_assert_ptr_nonnull(resp);
    ck_assert_str_eq(resp->content, "");
    llm_response_free(resp);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("LMStudio");
    TCase *tc = tcase_create("Streaming");
    tcase_add_test(tc, test_lmstudio_stream_accumulates_content_and_tool_calls);
    tcase_add_test(tc, test_lmstudio_stream_handles_empty_response);
    suite_add_tcase(suite, tc);
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
