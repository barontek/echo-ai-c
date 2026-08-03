#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <check.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "../../src/agent/message.h"
#include "../../src/llm/provider.h"
#include "../../src/utils/string_utils.h"

/* ---- Mirror structs from ollama.c (must stay in sync) ---- */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int thinking_open;
    void (*on_chunk)(const char *, void *);
    void *userdata;
    ToolCall *tool_calls;
    int tool_calls_count;
    int tool_calls_cap;
} WriteBuf;

/* ---- Wrappers exposed under OLLAMA_TEST in ollama.c ---- */
extern void ollama_test_parse_stream_tool_calls(WriteBuf *buf, cJSON *msg);
extern void ollama_test_forward_chunk(WriteBuf *buf, cJSON *msg);
extern LLMResponse *ollama_test_parse_response(const char *raw);
extern size_t ollama_test_write_cb(const char *data, size_t len, void *userdata);
extern char *ollama_test_build_url(const char *base_url);

/* ---- Curl stub globals (defined in ollama.c under OLLAMA_TEST) ---- */
extern char *ollama_test_captured_url;
extern char *ollama_test_captured_body;
extern int ollama_test_curl_init_fails;
extern CURLcode ollama_test_curl_result;

/* ---- Public create ---- */
extern LLMProvider *ollama_provider_create(const char *base_url, int num_ctx, int keep_alive_secs, const char *effort);

/* ---- Reset curl stub state ---- */
static void curl_stubs_reset(void)
{
    ollama_test_captured_url = NULL;
    ollama_test_captured_body = NULL;
    ollama_test_curl_init_fails = 0;
    ollama_test_curl_result = CURLE_OK;
}

/* ---- Shared chunk-collecting callback for forward_chunk tests ---- */
typedef struct {
    char *collected;
    size_t len;
    size_t cap;
} ChunkCollector;

static void collect_chunk(const char *chunk, void *userdata)
{
    ChunkCollector *cc = userdata;
    size_t clen = strlen(chunk);
    size_t needed = cc->len + clen + 1;
    if (needed > cc->cap)
    {
        cc->cap = needed * 2;
        char *new_data = realloc(cc->collected, cc->cap);
        if (!new_data) return;
        cc->collected = new_data;
    }
    memcpy(cc->collected + cc->len, chunk, clen + 1);
    cc->len += clen;
}

static void chunk_collector_free(ChunkCollector *cc)
{
    free(cc->collected);
    memset(cc, 0, sizeof(*cc));
}

/* ---- Helper: create messages array ---- */
static Message *make_messages(const char *role, const char *content)
{
    Message *m = calloc(1, sizeof(Message));
    if (m)
    {
        m->role = str_dup(role);
        m->content = str_dup(content);
    }
    return m;
}

static void free_messages(Message *msgs, int count)
{
    for (int i = 0; i < count; i++)
    {
        free(msgs[i].role);
        free(msgs[i].content);
    }
    free(msgs);
}

/* ================================================================
 *  Provider create / destroy tests
 * ================================================================ */

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

/* ================================================================
 *  ollama_parse_response tests
 * ================================================================ */

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

/* ================================================================
 *  forward_chunk tests
 * ================================================================ */

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

/* ================================================================
 *  build_url tests
 * ================================================================ */

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

/* ================================================================
 *  write_cb tests
 * ================================================================ */

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

/* ================================================================
 *  ollama_chat tests (via vtable, curl stubbed in ollama.c)
 * ================================================================ */

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

/* ================================================================
 *  ollama_chat_streaming tests (via vtable, curl stubbed)
 * ================================================================ */

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

/* ================================================================
 *  ollama_extract_structured tests (via vtable, curl stubbed)
 * ================================================================ */

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

/* ================================================================
 *  Suite
 * ================================================================ */

Suite *ollama_suite(void)
{
    Suite *s = suite_create("ollama");

    TCase *tc_create = tcase_create("ProviderCreateDestroy");
    tcase_add_test(tc_create, test_provider_create_stores_default_values);
    tcase_add_test(tc_create, test_provider_create_stores_custom_values);
    tcase_add_test(tc_create, test_provider_destroy_handles_null);
    suite_add_tcase(s, tc_create);

    TCase *tc_parse = tcase_create("ParseResponse");
    tcase_add_test(tc_parse, test_parse_response_extracts_content);
    tcase_add_test(tc_parse, test_parse_response_extracts_thinking_and_content);
    tcase_add_test(tc_parse, test_parse_response_returns_null_on_invalid_json);
    tcase_add_test(tc_parse, test_parse_response_returns_null_when_message_missing);
    tcase_add_test(tc_parse, test_parse_response_extracts_tool_calls);
    tcase_add_test(tc_parse, test_parse_response_empty_content_yields_empty_string);
    tcase_add_test(tc_parse, test_parse_response_thinking_only_creates_think_tags);
    tcase_add_test(tc_parse, test_parse_response_handles_empty_message);
    suite_add_tcase(s, tc_parse);

    TCase *tc_chunk = tcase_create("ForwardChunk");
    tcase_add_test(tc_chunk, test_forward_chunk_emits_content);
    tcase_add_test(tc_chunk, test_forward_chunk_emits_thinking_wrapped_in_tags);
    tcase_add_test(tc_chunk, test_forward_chunk_closes_thinking_before_content);
    tcase_add_test(tc_chunk, test_forward_chunk_skips_empty_content);
    tcase_add_test(tc_chunk, test_forward_chunk_skips_empty_thinking);
    tcase_add_test(tc_chunk, test_forward_chunk_emits_multiple_chunks_correctly);
    suite_add_tcase(s, tc_chunk);

    TCase *tc_url = tcase_create("BuildUrl");
    tcase_add_test(tc_url, test_build_url_constructs_api_chat_endpoint);
    tcase_add_test(tc_url, test_build_url_handles_trailing_slash);
    suite_add_tcase(s, tc_url);

    TCase *tc_write_cb = tcase_create("WriteCb");
    tcase_add_test(tc_write_cb, test_write_cb_accumulates_raw_data_without_chunk_callback);
    tcase_add_test(tc_write_cb, test_write_cb_parses_json_lines_and_calls_forward_chunk);
    tcase_add_test(tc_write_cb, test_write_cb_handles_multiple_json_lines);
    tcase_add_test(tc_write_cb, test_write_cb_buffers_partial_line_across_calls);
    tcase_add_test(tc_write_cb, test_write_cb_ignores_empty_lines);
    tcase_add_test(tc_write_cb, test_write_cb_stores_tool_calls_from_stream);
    suite_add_tcase(s, tc_write_cb);

    TCase *tc_chat = tcase_create("Chat");
    tcase_add_test(tc_chat, test_chat_builds_request_body_without_tools);
    tcase_add_test(tc_chat, test_chat_embeds_reasoning_effort_in_options);
    tcase_add_test(tc_chat, test_chat_omits_reasoning_effort_when_unset);
    tcase_add_test(tc_chat, test_provider_accepts_valid_effort_values);
    tcase_add_test(tc_chat, test_provider_rejects_invalid_effort);
    tcase_add_test(tc_chat, test_chat_returns_null_on_curl_init_failure);
    tcase_add_test(tc_chat, test_chat_returns_null_on_curl_perform_failure);
    tcase_add_test(tc_chat, test_chat_includes_tools_json_when_provided);
    suite_add_tcase(s, tc_chat);

    TCase *tc_stream = tcase_create("ChatStreaming");
    tcase_add_test(tc_stream, test_chat_streaming_builds_stream_true_body);
    tcase_add_test(tc_stream, test_chat_streaming_returns_null_on_curl_failure);
    tcase_add_test(tc_stream, test_chat_streaming_includes_tools_when_provided);
    suite_add_tcase(s, tc_stream);

    TCase *tc_extract = tcase_create("ExtractStructured");
    tcase_add_test(tc_extract, test_extract_structured_uses_format_json_by_default);
    tcase_add_test(tc_extract, test_extract_structured_uses_custom_json_schema);
    tcase_add_test(tc_extract, test_extract_structured_returns_null_on_curl_failure);
    suite_add_tcase(s, tc_extract);

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
