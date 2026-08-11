/*
 * test_ollama_fixture.h - shared fixtures for the ollama test
 * binaries: WriteBuf mirror, chunk collector, message helpers, and
 * curl-stub state. Split from test_ollama.c (2026-08 file-length
 * compliance).
 */

#ifndef ECHO_TEST_OLLAMA_FIXTURE_H
#define ECHO_TEST_OLLAMA_FIXTURE_H

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

typedef struct {
    char *collected;
    size_t len;
    size_t cap;
} ChunkCollector;

void collect_chunk(const char *chunk, void *userdata);
void chunk_collector_free(ChunkCollector *cc);
Message *make_messages(const char *role, const char *content);
void free_messages(Message *msgs, int count);
void curl_stubs_reset(void);

/* Wrappers exposed under OLLAMA_TEST in ollama.c */
void ollama_test_parse_stream_tool_calls(WriteBuf *buf, cJSON *msg);
void ollama_test_forward_chunk(WriteBuf *buf, cJSON *msg);
LLMResponse *ollama_test_parse_response(const char *raw);
size_t ollama_test_write_cb(const char *data, size_t len, void *userdata);
char *ollama_test_build_url(const char *base_url);
LLMProvider *ollama_provider_create(const char *base_url, int num_ctx, int keep_alive_secs, const char *effort);

/* Curl stub globals (defined in ollama.c under OLLAMA_TEST) */
extern char *ollama_test_captured_url;
extern char *ollama_test_captured_body;
extern int ollama_test_curl_init_fails;
extern CURLcode ollama_test_curl_result;

#endif /* ECHO_TEST_OLLAMA_FIXTURE_H */
