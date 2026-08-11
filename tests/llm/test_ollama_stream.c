/*
 * test_ollama_stream.c - ['test_forward_chunk_emits_content', 'test_forward_chunk_emits_thinking_wrapped_in_tags', 'test_forward_chunk_closes_thinking_before_content', 'test_forward_chunk_skips_empty_content', 'test_forward_chunk_skips_empty_thinking', 'test_forward_chunk_emits_multiple_chunks_correctly'] tests for the ollama provider.
 * Split from test_ollama.c (2026-08 file-length compliance).
 * Depends on: check, ollama under OLLAMA_TEST.
 */

#include "test_ollama_fixture.h"

START_TEST(test_forward_chunk_emits_content)
{
    ChunkCollector cc = {0};
    WriteBuf buf = {0};
    buf.on_chunk = collect_chunk;
    buf.userdata = &cc;

    cJSON *msg = cJSON_Parse("{\"content\":\"Hello\"}");
    ck_assert_ptr_ne(msg, NULL);

    ollama_test_forward_chunk(&buf, msg);

    ck_assert_str_eq(cc.collected, "Hello");

    cJSON_Delete(msg);
    chunk_collector_free(&cc);
}
END_TEST

START_TEST(test_forward_chunk_emits_thinking_wrapped_in_tags)
{
    ChunkCollector cc = {0};
    WriteBuf buf = {0};
    buf.on_chunk = collect_chunk;
    buf.userdata = &cc;

    cJSON *msg = cJSON_Parse("{\"thinking\":\"reasoning\"}");
    ck_assert_ptr_ne(msg, NULL);

    ollama_test_forward_chunk(&buf, msg);

    ck_assert_str_eq(cc.collected, "<think>\nreasoning");

    cJSON_Delete(msg);
    chunk_collector_free(&cc);
}
END_TEST

START_TEST(test_forward_chunk_closes_thinking_before_content)
{
    ChunkCollector cc = {0};
    WriteBuf buf = {0};
    buf.on_chunk = collect_chunk;
    buf.userdata = &cc;
    buf.thinking_open = 1;

    cJSON *msg = cJSON_Parse("{\"content\":\"answer\"}");
    ck_assert_ptr_ne(msg, NULL);

    ollama_test_forward_chunk(&buf, msg);

    ck_assert(strstr(cc.collected, "</think>") != NULL);
    ck_assert(strstr(cc.collected, "answer") != NULL);

    cJSON_Delete(msg);
    chunk_collector_free(&cc);
}
END_TEST

START_TEST(test_forward_chunk_skips_empty_content)
{
    ChunkCollector cc = {0};
    WriteBuf buf = {0};
    buf.on_chunk = collect_chunk;
    buf.userdata = &cc;

    cJSON *msg = cJSON_Parse("{\"content\":\"\"}");
    ck_assert_ptr_ne(msg, NULL);

    ollama_test_forward_chunk(&buf, msg);

    ck_assert_ptr_eq(cc.collected, NULL);

    cJSON_Delete(msg);
    chunk_collector_free(&cc);
}
END_TEST

START_TEST(test_forward_chunk_skips_empty_thinking)
{
    ChunkCollector cc = {0};
    WriteBuf buf = {0};
    buf.on_chunk = collect_chunk;
    buf.userdata = &cc;

    cJSON *msg = cJSON_Parse("{\"thinking\":\"\"}");
    ck_assert_ptr_ne(msg, NULL);

    ollama_test_forward_chunk(&buf, msg);

    ck_assert_ptr_eq(cc.collected, NULL);

    cJSON_Delete(msg);
    chunk_collector_free(&cc);
}
END_TEST

START_TEST(test_forward_chunk_emits_multiple_chunks_correctly)
{
    ChunkCollector cc = {0};
    WriteBuf buf = {0};
    buf.on_chunk = collect_chunk;
    buf.userdata = &cc;

    cJSON *msg1 = cJSON_Parse("{\"thinking\":\"step1\"}");
    cJSON *msg2 = cJSON_Parse("{\"thinking\":\"step2\"}");
    cJSON *msg3 = cJSON_Parse("{\"content\":\"result\"}");

    ollama_test_forward_chunk(&buf, msg1);
    ollama_test_forward_chunk(&buf, msg2);
    ollama_test_forward_chunk(&buf, msg3);

    ck_assert(strstr(cc.collected, "<think>") != NULL);
    ck_assert(strstr(cc.collected, "step1") != NULL);
    ck_assert(strstr(cc.collected, "step2") != NULL);
    ck_assert(strstr(cc.collected, "</think>") != NULL);
    ck_assert(strstr(cc.collected, "result") != NULL);

    cJSON_Delete(msg1);
    cJSON_Delete(msg2);
    cJSON_Delete(msg3);
    chunk_collector_free(&cc);
}
END_TEST

START_TEST(test_write_cb_accumulates_raw_data_without_chunk_callback)
{
    WriteBuf buf = {0};

    size_t ret = ollama_test_write_cb("Hello, ", 7, &buf);
    ck_assert_int_eq(ret, 7);
    ck_assert_str_eq(buf.data, "Hello, ");

    ret = ollama_test_write_cb("world!", 6, &buf);
    ck_assert_int_eq(ret, 6);
    ck_assert_str_eq(buf.data, "Hello, world!");

    free(buf.data);
}
END_TEST

START_TEST(test_write_cb_parses_json_lines_and_calls_forward_chunk)
{
    ChunkCollector cc = {0};
    WriteBuf buf = {0};
    buf.on_chunk = collect_chunk;
    buf.userdata = &cc;

    ollama_test_write_cb(
        "{\"message\":{\"content\":\"Hello\"}}\n",
        strlen("{\"message\":{\"content\":\"Hello\"}}\n"), &buf);

    ck_assert_str_eq(cc.collected, "Hello");

    chunk_collector_free(&cc);
    free(buf.data);
}
END_TEST

START_TEST(test_write_cb_handles_multiple_json_lines)
{
    ChunkCollector cc = {0};
    WriteBuf buf = {0};
    buf.on_chunk = collect_chunk;
    buf.userdata = &cc;

    const char *two_lines =
        "{\"message\":{\"content\":\"A\"}}\n"
        "{\"message\":{\"content\":\"B\"}}\n";
    ollama_test_write_cb(two_lines, strlen(two_lines), &buf);

    ck_assert_str_eq(cc.collected, "AB");

    chunk_collector_free(&cc);
    free(buf.data);
}
END_TEST

START_TEST(test_write_cb_buffers_partial_line_across_calls)
{
    ChunkCollector cc = {0};
    WriteBuf buf = {0};
    buf.on_chunk = collect_chunk;
    buf.userdata = &cc;

    ollama_test_write_cb(
        "{\"message\":{\"content\":\"He",
        strlen("{\"message\":{\"content\":\"He"), &buf);

    ck_assert_ptr_eq(cc.collected, NULL);

    ollama_test_write_cb(
        "llo\"}}\n", strlen("llo\"}}\n"), &buf);

    ck_assert_str_eq(cc.collected, "Hello");

    chunk_collector_free(&cc);
    free(buf.data);
}
END_TEST

START_TEST(test_write_cb_ignores_empty_lines)
{
    ChunkCollector cc = {0};
    WriteBuf buf = {0};
    buf.on_chunk = collect_chunk;
    buf.userdata = &cc;

    const char *empty_lines = "\n\n{\"message\":{\"content\":\"X\"}}\n\n";
    ollama_test_write_cb(empty_lines, strlen(empty_lines), &buf);

    ck_assert_str_eq(cc.collected, "X");

    chunk_collector_free(&cc);
    free(buf.data);
}
END_TEST

START_TEST(test_write_cb_stores_tool_calls_from_stream)
{
    WriteBuf buf = {0};
    ChunkCollector cc = {0};
    buf.on_chunk = collect_chunk;
    buf.userdata = &cc;

    const char *tool_call_stream =
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"bash\",\"arguments\":{\"cmd\":\"ls\"}}}"
        "]}}\n";
    ollama_test_write_cb(tool_call_stream, strlen(tool_call_stream), &buf);

    ck_assert_int_eq(buf.tool_calls_count, 1);
    ck_assert_str_eq(buf.tool_calls[0].name, "bash");

    tool_call_free(&buf.tool_calls[0]);
    free(buf.tool_calls);
    chunk_collector_free(&cc);
    free(buf.data);
}
END_TEST

START_TEST(test_chat_streaming_builds_stream_true_body)
{
    curl_stubs_reset();

    LLMProvider *p = ollama_provider_create("http://localhost:11434", 4096, 120, NULL);
    ck_assert_ptr_ne(p, NULL);

    Message *msgs = make_messages("user", "hello");
    ck_assert_ptr_ne(msgs, NULL);

    LLMResponse *r = p->chat_streaming(p, msgs, 1, "llama3", 0.7, 30,
                                        NULL, NULL, NULL);
    /* streaming with no data → LLMResponse with NULL content */
    ck_assert_ptr_ne(r, NULL);
    ck_assert_ptr_eq(r->content, NULL);

    ck_assert_ptr_ne(ollama_test_captured_body, NULL);
    ck_assert(strstr(ollama_test_captured_body, "\"stream\":true") != NULL);

    llm_response_free(r);
    free_messages(msgs, 1);
    p->destroy(p);
}
END_TEST

START_TEST(test_chat_streaming_returns_null_on_curl_failure)
{
    curl_stubs_reset();
    ollama_test_curl_result = (CURLcode)1;

    LLMProvider *p = ollama_provider_create("http://localhost:11434", 4096, 120, NULL);
    ck_assert_ptr_ne(p, NULL);

    Message *msgs = make_messages("user", "hello");
    ck_assert_ptr_ne(msgs, NULL);

    LLMResponse *r = p->chat_streaming(p, msgs, 1, "llama3", 0.7, 30,
                                        NULL, NULL, NULL);
    ck_assert_ptr_eq(r, NULL);

    free_messages(msgs, 1);
    p->destroy(p);
}
END_TEST

START_TEST(test_chat_streaming_includes_tools_when_provided)
{
    curl_stubs_reset();

    LLMProvider *p = ollama_provider_create("http://localhost:11434", 4096, 120, NULL);
    ck_assert_ptr_ne(p, NULL);

    Message *msgs = make_messages("user", "run ls");
    ck_assert_ptr_ne(msgs, NULL);

    LLMResponse *r = p->chat_streaming(p, msgs, 1, "llama3", 0.7, 30,
                                        NULL, NULL,
                                        "[{\"type\":\"function\",\"function\":{\"name\":\"bash\"}}]");
    /* streaming with no data → LLMResponse with NULL content */
    ck_assert_ptr_ne(r, NULL);

    ck_assert_ptr_ne(ollama_test_captured_body, NULL);
    ck_assert(strstr(ollama_test_captured_body, "\"stream\":true") != NULL);
    ck_assert(strstr(ollama_test_captured_body, "\"tools\":") != NULL);

    llm_response_free(r);
    free_messages(msgs, 1);
    p->destroy(p);
}
END_TEST

Suite *ollama_suite(void)
{
    Suite *s = suite_create("ollama");

    TCase *tc = tcase_create("ForwardChunk");
    tcase_add_test(tc, test_forward_chunk_emits_content);
    tcase_add_test(tc, test_forward_chunk_emits_thinking_wrapped_in_tags);
    tcase_add_test(tc, test_forward_chunk_closes_thinking_before_content);
    tcase_add_test(tc, test_forward_chunk_skips_empty_content);
    tcase_add_test(tc, test_forward_chunk_skips_empty_thinking);
    tcase_add_test(tc, test_forward_chunk_emits_multiple_chunks_correctly);
    suite_add_tcase(s, tc);

    tc = tcase_create("WriteCb");
    tcase_add_test(tc, test_write_cb_accumulates_raw_data_without_chunk_callback);
    tcase_add_test(tc, test_write_cb_parses_json_lines_and_calls_forward_chunk);
    tcase_add_test(tc, test_write_cb_handles_multiple_json_lines);
    tcase_add_test(tc, test_write_cb_buffers_partial_line_across_calls);
    tcase_add_test(tc, test_write_cb_ignores_empty_lines);
    tcase_add_test(tc, test_write_cb_stores_tool_calls_from_stream);
    suite_add_tcase(s, tc);

    tc = tcase_create("ChatStreaming");
    tcase_add_test(tc, test_chat_streaming_builds_stream_true_body);
    tcase_add_test(tc, test_chat_streaming_returns_null_on_curl_failure);
    tcase_add_test(tc, test_chat_streaming_includes_tools_when_provided);
    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s = ollama_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}
