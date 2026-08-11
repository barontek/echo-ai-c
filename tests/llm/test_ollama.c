/*
 * test_ollama.c - ['test_provider_create_stores_default_values', 'test_provider_create_stores_custom_values', 'test_provider_destroy_handles_null'] tests for the ollama provider.
 * Split from test_ollama.c (2026-08 file-length compliance).
 * Depends on: check, ollama under OLLAMA_TEST.
 */

#include "test_ollama_fixture.h"

START_TEST(test_provider_create_stores_default_values)
{
    LLMProvider *p = ollama_provider_create(NULL, 0, 0, NULL);
    ck_assert_ptr_ne(p, NULL);
    ck_assert(p->chat != NULL);
    ck_assert(p->chat_streaming != NULL);
    ck_assert(p->extract_structured != NULL);
    ck_assert(p->destroy != NULL);
    ck_assert_ptr_ne(p->ctx, NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_provider_create_stores_custom_values)
{
    LLMProvider *p = ollama_provider_create("http://127.0.0.1:9999", 8192, 300, NULL);
    ck_assert_ptr_ne(p, NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_provider_destroy_handles_null)
{
    LLMProvider *p = ollama_provider_create(NULL, 0, 0, NULL);
    ck_assert_ptr_ne(p, NULL);
    p->destroy(NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_parse_response_extracts_content)
{
    LLMResponse *r = ollama_test_parse_response(
        "{\"message\":{\"role\":\"assistant\",\"content\":\"Hello world\"}}");
    ck_assert_ptr_ne(r, NULL);
    ck_assert_str_eq(r->content, "Hello world");
    ck_assert_ptr_eq(r->thinking, NULL);
    ck_assert_int_eq(r->tool_calls_count, 0);
    llm_response_free(r);
}
END_TEST

START_TEST(test_parse_response_extracts_thinking_and_content)
{
    LLMResponse *r = ollama_test_parse_response(
        "{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Sure, here is the code\","
        "\"thinking\":\"Let me reason through this step by step\"}}");
    ck_assert_ptr_ne(r, NULL);
    ck_assert_ptr_ne(r->content, NULL);
    ck_assert(strstr(r->content, "<think>") != NULL);
    ck_assert(strstr(r->content, "Let me reason") != NULL);
    ck_assert(strstr(r->content, "</think>") != NULL);
    ck_assert(strstr(r->content, "Sure, here is the code") != NULL);
    llm_response_free(r);
}
END_TEST

START_TEST(test_parse_response_returns_null_on_invalid_json)
{
    LLMResponse *r = ollama_test_parse_response("not valid json");
    ck_assert_ptr_eq(r, NULL);
}
END_TEST

START_TEST(test_parse_response_returns_null_when_message_missing)
{
    LLMResponse *r = ollama_test_parse_response("{\"no_message\":1}");
    ck_assert_ptr_eq(r, NULL);
}
END_TEST

START_TEST(test_parse_response_extracts_tool_calls)
{
    LLMResponse *r = ollama_test_parse_response(
        "{\"message\":{"
        "\"content\":\"I'll run that\","
        "\"tool_calls\":[{\"function\":{\"name\":\"bash\",\"arguments\":{\"cmd\":\"ls\"}}}]"
        "}}");
    ck_assert_ptr_ne(r, NULL);
    ck_assert_int_eq(r->tool_calls_count, 1);
    ck_assert_str_eq(r->tool_calls[0].name, "bash");
    ck_assert_ptr_ne(r->tool_calls[0].id, NULL);
    ck_assert_ptr_ne(r->tool_calls[0].arguments, NULL);
    ck_assert_str_eq(r->content, "I'll run that");
    llm_response_free(r);
}
END_TEST

START_TEST(test_parse_response_empty_content_yields_empty_string)
{
    LLMResponse *r = ollama_test_parse_response(
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"}}");
    ck_assert_ptr_ne(r, NULL);
    ck_assert_str_eq(r->content, "");
    llm_response_free(r);
}
END_TEST

START_TEST(test_parse_response_thinking_only_creates_think_tags)
{
    LLMResponse *r = ollama_test_parse_response(
        "{\"message\":{\"thinking\":\"deep thoughts\",\"content\":\"\"}}");
    ck_assert_ptr_ne(r, NULL);
    ck_assert_ptr_ne(r->content, NULL);
    ck_assert(strstr(r->content, "<think>") != NULL);
    ck_assert(strstr(r->content, "deep thoughts") != NULL);
    ck_assert(strstr(r->content, "</think>") != NULL);
    llm_response_free(r);
}
END_TEST

START_TEST(test_parse_response_handles_empty_message)
{
    LLMResponse *r = ollama_test_parse_response("{\"message\":{}}");
    ck_assert_ptr_ne(r, NULL);
    ck_assert_ptr_eq(r->content, NULL);
    ck_assert_ptr_eq(r->thinking, NULL);
    ck_assert_int_eq(r->tool_calls_count, 0);
    llm_response_free(r);
}
END_TEST

START_TEST(test_build_url_constructs_api_chat_endpoint)
{
    char *url = ollama_test_build_url("http://localhost:11434");
    ck_assert_ptr_ne(url, NULL);
    ck_assert_str_eq(url, "http://localhost:11434/api/chat");
    free(url);
}
END_TEST

START_TEST(test_build_url_handles_trailing_slash)
{
    char *url = ollama_test_build_url("http://localhost:11434/");
    ck_assert_ptr_ne(url, NULL);
    ck_assert_str_eq(url, "http://localhost:11434//api/chat");
    free(url);
}
END_TEST

START_TEST(test_chat_builds_request_body_without_tools)
{
    curl_stubs_reset();

    LLMProvider *p = ollama_provider_create("http://localhost:11434", 4096, 120, NULL);
    ck_assert_ptr_ne(p, NULL);

    Message *msgs = make_messages("user", "hello");
    ck_assert_ptr_ne(msgs, NULL);

    LLMResponse *r = p->chat(p, msgs, 1, "llama3", 0.7, 30, NULL);
    /* stub returns no data → raw is empty → parse returns NULL */
    ck_assert_ptr_eq(r, NULL);

    ck_assert_ptr_ne(ollama_test_captured_body, NULL);
    ck_assert(strstr(ollama_test_captured_body, "\"model\":\"llama3\"") != NULL);
    ck_assert(strstr(ollama_test_captured_body, "\"stream\":false") != NULL);
    ck_assert(strstr(ollama_test_captured_body, "\"keep_alive\":120") != NULL);
    ck_assert(strstr(ollama_test_captured_body, "\"num_ctx\":4096") != NULL);

    free_messages(msgs, 1);
    p->destroy(p);
}
END_TEST

START_TEST(test_chat_embeds_reasoning_effort_in_options)
{
    curl_stubs_reset();

    LLMProvider *p = ollama_provider_create("http://localhost:11434", 4096, 120, "max");
    ck_assert_ptr_ne(p, NULL);

    Message *msgs = make_messages("user", "hello");
    ck_assert_ptr_ne(msgs, NULL);

    LLMResponse *r = p->chat(p, msgs, 1, "qwen3", 0.7, 30, NULL);
    ck_assert_ptr_eq(r, NULL); /* stub returns no data */

    ck_assert_ptr_ne(ollama_test_captured_body, NULL);
    ck_assert(strstr(ollama_test_captured_body, "\"reasoning_effort\":\"max\"") != NULL);
    ck_assert(strstr(ollama_test_captured_body, "\"options\":{") != NULL);

    free_messages(msgs, 1);
    p->destroy(p);
}
END_TEST

START_TEST(test_chat_omits_reasoning_effort_when_unset)
{
    curl_stubs_reset();

    LLMProvider *p = ollama_provider_create("http://localhost:11434", 4096, 120, NULL);
    ck_assert_ptr_ne(p, NULL);

    Message *msgs = make_messages("user", "hello");
    ck_assert_ptr_ne(msgs, NULL);

    LLMResponse *r = p->chat(p, msgs, 1, "llama3", 0.7, 30, NULL);
    ck_assert_ptr_eq(r, NULL);

    ck_assert_ptr_ne(ollama_test_captured_body, NULL);
    ck_assert_ptr_null(strstr(ollama_test_captured_body, "reasoning_effort"));

    free_messages(msgs, 1);
    p->destroy(p);
}
END_TEST

START_TEST(test_provider_accepts_valid_effort_values)
{
    const char *valid[] = {"low", "medium", "high", "max", "none"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
    {
        LLMProvider *p = ollama_provider_create(NULL, 0, 0, valid[i]);
        ck_assert_ptr_ne(p, NULL);
        p->destroy(p);
    }
}
END_TEST

START_TEST(test_provider_rejects_invalid_effort)
{
    /* xhigh is openai-only; minimal was dropped repo-wide. */
    ck_assert_ptr_null(ollama_provider_create(NULL, 0, 0, "xhigh"));
    ck_assert_ptr_null(ollama_provider_create(NULL, 0, 0, "minimal"));
    ck_assert_ptr_null(ollama_provider_create(NULL, 0, 0, "extreme"));
}
END_TEST

START_TEST(test_chat_returns_null_on_curl_init_failure)
{
    curl_stubs_reset();
    ollama_test_curl_init_fails = 1;

    LLMProvider *p = ollama_provider_create("http://localhost:11434", 4096, 120, NULL);
    ck_assert_ptr_ne(p, NULL);

    Message *msgs = make_messages("user", "hello");
    ck_assert_ptr_ne(msgs, NULL);

    LLMResponse *r = p->chat(p, msgs, 1, "llama3", 0.7, 30, NULL);
    ck_assert_ptr_eq(r, NULL);

    free_messages(msgs, 1);
    p->destroy(p);
}
END_TEST

START_TEST(test_chat_returns_null_on_curl_perform_failure)
{
    curl_stubs_reset();
    ollama_test_curl_result = (CURLcode)1;

    LLMProvider *p = ollama_provider_create("http://localhost:11434", 4096, 120, NULL);
    ck_assert_ptr_ne(p, NULL);

    Message *msgs = make_messages("user", "hello");
    ck_assert_ptr_ne(msgs, NULL);

    LLMResponse *r = p->chat(p, msgs, 1, "llama3", 0.7, 30, NULL);
    ck_assert_ptr_eq(r, NULL);

    free_messages(msgs, 1);
    p->destroy(p);
}
END_TEST

START_TEST(test_chat_includes_tools_json_when_provided)
{
    curl_stubs_reset();

    LLMProvider *p = ollama_provider_create("http://localhost:11434", 4096, 120, NULL);
    ck_assert_ptr_ne(p, NULL);

    Message *msgs = make_messages("user", "run ls");
    ck_assert_ptr_ne(msgs, NULL);

    LLMResponse *r = p->chat(p, msgs, 1, "llama3", 0.7, 30,
                             "[{\"type\":\"function\",\"function\":{\"name\":\"bash\"}}]");
    ck_assert_ptr_eq(r, NULL);

    ck_assert_ptr_ne(ollama_test_captured_body, NULL);
    ck_assert(strstr(ollama_test_captured_body, "\"tools\":[{\"type\":\"function\"") != NULL);

    free_messages(msgs, 1);
    p->destroy(p);
}
END_TEST

START_TEST(test_extract_structured_uses_format_json_by_default)
{
    curl_stubs_reset();

    LLMProvider *p = ollama_provider_create("http://localhost:11434", 4096, 120, NULL);
    ck_assert_ptr_ne(p, NULL);

    Message *msgs = make_messages("user", "extract name");
    ck_assert_ptr_ne(msgs, NULL);

    LLMResponse *r = p->extract_structured(p, msgs, 1, "llama3", 0.0, 30, NULL);
    ck_assert_ptr_eq(r, NULL);

    ck_assert_ptr_ne(ollama_test_captured_body, NULL);
    ck_assert(strstr(ollama_test_captured_body, "\"format\":\"json\"") != NULL);

    free_messages(msgs, 1);
    p->destroy(p);
}
END_TEST

START_TEST(test_extract_structured_uses_custom_json_schema)
{
    curl_stubs_reset();

    LLMProvider *p = ollama_provider_create("http://localhost:11434", 4096, 120, NULL);
    ck_assert_ptr_ne(p, NULL);

    Message *msgs = make_messages("user", "extract data");
    ck_assert_ptr_ne(msgs, NULL);

    LLMResponse *r = p->extract_structured(p, msgs, 1, "llama3", 0.0, 30,
                                            "{\"type\":\"object\"}");
    ck_assert_ptr_eq(r, NULL);

    ck_assert_ptr_ne(ollama_test_captured_body, NULL);
    ck_assert(strstr(ollama_test_captured_body, "\"format\":{\"type\":\"object\"}") != NULL);

    free_messages(msgs, 1);
    p->destroy(p);
}
END_TEST

START_TEST(test_extract_structured_returns_null_on_curl_failure)
{
    curl_stubs_reset();
    ollama_test_curl_result = (CURLcode)1;

    LLMProvider *p = ollama_provider_create("http://localhost:11434", 4096, 120, NULL);
    ck_assert_ptr_ne(p, NULL);

    Message *msgs = make_messages("user", "hello");
    ck_assert_ptr_ne(msgs, NULL);

    LLMResponse *r = p->extract_structured(p, msgs, 1, "llama3", 0.0, 30, NULL);
    ck_assert_ptr_eq(r, NULL);

    free_messages(msgs, 1);
    p->destroy(p);
}
END_TEST

Suite *ollama_suite(void)
{
    Suite *s = suite_create("ollama");

    TCase *tc = tcase_create("ProviderCreateDestroy");
    tcase_add_test(tc, test_provider_create_stores_default_values);
    tcase_add_test(tc, test_provider_create_stores_custom_values);
    tcase_add_test(tc, test_provider_destroy_handles_null);
    suite_add_tcase(s, tc);

    tc = tcase_create("ParseResponse");
    tcase_add_test(tc, test_parse_response_extracts_content);
    tcase_add_test(tc, test_parse_response_extracts_thinking_and_content);
    tcase_add_test(tc, test_parse_response_returns_null_on_invalid_json);
    tcase_add_test(tc, test_parse_response_returns_null_when_message_missing);
    tcase_add_test(tc, test_parse_response_extracts_tool_calls);
    tcase_add_test(tc, test_parse_response_empty_content_yields_empty_string);
    tcase_add_test(tc, test_parse_response_thinking_only_creates_think_tags);
    tcase_add_test(tc, test_parse_response_handles_empty_message);
    suite_add_tcase(s, tc);

    tc = tcase_create("BuildUrl");
    tcase_add_test(tc, test_build_url_constructs_api_chat_endpoint);
    tcase_add_test(tc, test_build_url_handles_trailing_slash);
    suite_add_tcase(s, tc);

    tc = tcase_create("Chat");
    tcase_add_test(tc, test_chat_builds_request_body_without_tools);
    tcase_add_test(tc, test_chat_embeds_reasoning_effort_in_options);
    tcase_add_test(tc, test_chat_omits_reasoning_effort_when_unset);
    tcase_add_test(tc, test_provider_accepts_valid_effort_values);
    tcase_add_test(tc, test_provider_rejects_invalid_effort);
    tcase_add_test(tc, test_chat_returns_null_on_curl_init_failure);
    tcase_add_test(tc, test_chat_returns_null_on_curl_perform_failure);
    tcase_add_test(tc, test_chat_includes_tools_json_when_provided);
    suite_add_tcase(s, tc);

    tc = tcase_create("ExtractStructured");
    tcase_add_test(tc, test_extract_structured_uses_format_json_by_default);
    tcase_add_test(tc, test_extract_structured_uses_custom_json_schema);
    tcase_add_test(tc, test_extract_structured_returns_null_on_curl_failure);
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
