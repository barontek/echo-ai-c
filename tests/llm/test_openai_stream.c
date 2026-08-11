/*
 * test_openai_stream.c - SSE streaming tests for the Codex provider
 * (fragment buffering, tool-call folding, reasoning summaries).
 * Split from test_openai.c (2026-08 file-length compliance).
 * Depends on: check, openai under OPENAI_TEST.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "llm/openai.h"
#include "llm/openai_oauth.h"
#include "utils/logging.h"

void log_msg(LogLevel level, const char *file, int line,
             const char *message, ...)
{
    (void)level;
    (void)file;
    (void)line;
    (void)message;
}

/* OAuth stubs mirroring test_openai.c: the response unit's credential
 * helpers never succeed in this binary, which is fine for stream tests. */
struct OpenAIOAuth {
    int refresh_result;
};

static int refresh_calls = 0;
static int get_token_calls = 0;

static char *test_dup(const char *value)
{
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy) memcpy(copy, value, length);
    return copy;
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
    (void)rejected_token;
    if (!auth || auth->refresh_result != 0)
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
    LLMResponse *response = openai_test_stream_fragments_alloc(
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

START_TEST(test_stream_wraps_reasoning_summary_deltas_in_think_block)
{
    const char *stream =
        "data: {\"type\":\"response.reasoning_summary_text.delta\","
        "\"delta\":\"Checking docs\",\"summary_index\":0}\n\n"
        "data: {\"type\":\"response.reasoning_summary_text.delta\","
        "\"delta\":\" before replying\",\"summary_index\":0}\n\n"
        "data: {\"type\":\"response.reasoning_summary_text.done\","
        "\"text\":\"Checking docs before replying\",\"summary_index\":0}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,"
        "\"item\":{\"type\":\"reasoning\",\"id\":\"reason-1\","
        "\"summary\":[{\"type\":\"summary_text\","
        "\"text\":\"Checking docs before replying\"}]}}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Done.\"}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":1,"
        "\"item\":{\"type\":\"message\",\"role\":\"assistant\","
        "\"phase\":\"final_answer\",\"content\":[]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"status\":\"completed\"}}";
    ChunkCapture capture = {0};
    LLMResponse *response = openai_test_stream_fragments_alloc(
        &stream, NULL, 1, capture_chunk, &capture);
    ck_assert_ptr_nonnull(response);
    /* deltas are streamed inside the tags; the .done text is not appended
     * again because it would duplicate what the deltas already carried */
    ck_assert_str_eq(response->content,
                     "<think>\nChecking docs before replying\n</think>\n\nDone.");
    ck_assert_str_eq(capture.text,
                     "<think>\nChecking docs before replying\n</think>\n\nDone.");
    llm_response_free(response);
}
END_TEST

START_TEST(test_stream_uses_summary_done_text_when_no_deltas)
{
    /* backends using cutoff delivery send only the .done event carrying the
     * full summary text */
    const char *stream =
        "data: {\"type\":\"response.reasoning_summary_text.done\","
        "\"text\":\"Full summary\",\"summary_index\":0}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,"
        "\"item\":{\"type\":\"reasoning\",\"id\":\"r1\","
        "\"summary\":[{\"type\":\"summary_text\",\"text\":\"Full summary\"}]}}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Answer\"}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"status\":\"completed\"}}";
    LLMResponse *response = openai_test_stream_fragments_alloc(
        &stream, NULL, 1, NULL, NULL);
    ck_assert_ptr_nonnull(response);
    ck_assert_str_eq(response->content,
                     "<think>\nFull summary\n</think>\n\nAnswer");
    llm_response_free(response);
}
END_TEST

START_TEST(test_stream_falls_back_to_reasoning_item_summary)
{
    /* a backend that emits neither delta nor done still carries the final
     * summary on the reasoning output item itself */
    const char *stream =
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,"
        "\"item\":{\"type\":\"reasoning\",\"id\":\"r1\"}}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,"
        "\"item\":{\"type\":\"reasoning\",\"id\":\"r1\","
        "\"summary\":[{\"type\":\"summary_text\",\"text\":\"Item summary\"}]}}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Answer\"}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"status\":\"completed\"}}";
    LLMResponse *response = openai_test_stream_fragments_alloc(
        &stream, NULL, 1, NULL, NULL);
    ck_assert_ptr_nonnull(response);
    ck_assert_str_eq(response->content,
                     "<think>\nItem summary\n</think>\n\nAnswer");
    llm_response_free(response);
}
END_TEST

START_TEST(test_stream_strips_html_comment_markers_from_summaries)
{
    /* codex can elide summaries to a literal <!-- --> placeholder (see
     * openai/codex#31664); it must never render as an empty think block */
    const char *stream =
        "data: {\"type\":\"response.reasoning_summary_text.delta\","
        "\"delta\":\"<!-- -->\",\"summary_index\":0}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,"
        "\"item\":{\"type\":\"reasoning\",\"id\":\"r1\","
        "\"summary\":[{\"type\":\"summary_text\",\"text\":\"<!-- -->\"}]}}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Answer\"}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"status\":\"completed\"}}";
    LLMResponse *response = openai_test_stream_fragments_alloc(
        &stream, NULL, 1, NULL, NULL);
    ck_assert_ptr_nonnull(response);
    ck_assert_str_eq(response->content, "Answer");
    llm_response_free(response);
}
END_TEST

START_TEST(test_stream_keeps_think_block_open_across_tool_turns)
{
    /* a tool-only turn ends with the reasoning block still open; the tag
     * must be closed so the saved message parses cleanly */
    const char *stream =
        "data: {\"type\":\"response.reasoning_summary_text.delta\","
        "\"delta\":\"Need the file\",\"summary_index\":0}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,"
        "\"item\":{\"type\":\"reasoning\",\"id\":\"r1\","
        "\"summary\":[{\"type\":\"summary_text\",\"text\":\"Need the file\"}]}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":1,"
        "\"item\":{\"type\":\"function_call\",\"id\":\"item-a\","
        "\"call_id\":\"call-a\",\"name\":\"read_file\",\"arguments\":\"\"}}\n\n"
        "data: {\"type\":\"response.function_call_arguments.done\","
        "\"output_index\":1,\"item_id\":\"item-a\","
        "\"arguments\":\"{\\\"path\\\":\\\"a.txt\\\"}\"}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":1,"
        "\"item\":{\"type\":\"function_call\",\"id\":\"item-a\","
        "\"call_id\":\"call-a\",\"name\":\"read_file\","
        "\"arguments\":\"{\\\"path\\\":\\\"a.txt\\\"}\"}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"status\":\"completed\"}}";
    LLMResponse *response = openai_test_stream_fragments_alloc(
        &stream, NULL, 1, NULL, NULL);
    ck_assert_ptr_nonnull(response);
    ck_assert_str_eq(response->content,
                     "<think>\nNeed the file\n</think>\n\n");
    ck_assert_int_eq(response->tool_calls_count, 1);
    ck_assert_str_eq(response->tool_calls[0].name, "read_file");
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
    ck_assert_ptr_null(openai_test_stream_fragments_alloc(&failed, NULL, 1, NULL, NULL));
    ck_assert_ptr_null(openai_test_stream_fragments_alloc(&incomplete, NULL, 1, NULL, NULL));
}
END_TEST

START_TEST(test_stream_accepts_fragmented_done_without_final_newline)
{
    const char *fragments[] = {
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"o",
        "k\"}\n\ndata: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":[]}}\n\ndata: [DO",
        "NE]"};
    LLMResponse *response = openai_test_stream_fragments_alloc(
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
    ck_assert_ptr_null(openai_test_stream_fragments_alloc(
        &done_only, NULL, 1, NULL, NULL));
    LLMResponse *response = openai_test_stream_fragments_alloc(
        &refusal, NULL, 1, NULL, NULL);
    ck_assert_ptr_nonnull(response);
    ck_assert_str_eq(response->content, "no");
    llm_response_free(response);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("OpenAI Codex Stream");

    TCase *stream = tcase_create("Streaming Responses");
    tcase_set_timeout(stream, 5);
    tcase_add_test(stream, test_fragmented_stream_maps_interleaved_function_calls);
    tcase_add_test(stream, test_stream_wraps_reasoning_summary_deltas_in_think_block);
    tcase_add_test(stream, test_stream_uses_summary_done_text_when_no_deltas);
    tcase_add_test(stream, test_stream_falls_back_to_reasoning_item_summary);
    tcase_add_test(stream, test_stream_strips_html_comment_markers_from_summaries);
    tcase_add_test(stream, test_stream_keeps_think_block_open_across_tool_turns);
    tcase_add_test(stream, test_stream_requires_successful_terminal_event);
    tcase_add_test(stream, test_stream_accepts_fragmented_done_without_final_newline);
    tcase_add_test(stream, test_stream_rejects_done_without_completed_and_returns_refusals);
    suite_add_tcase(suite, stream);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed == 0 ? 0 : 1;
}
