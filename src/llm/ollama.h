/*
 * ollama.h - Ollama provider: construction and effort validation for
 * the /api/chat backend, plus test hooks under OLLAMA_TEST.
 * Depends on: provider.h (LLMProvider).
 */

#ifndef ECHO_OLLAMA_H
#define ECHO_OLLAMA_H

#include "provider.h"

/**
 * ollama_provider_create - construct the Ollama LLM provider
 * @base_url: borrowed endpoint (e.g. "http://localhost:11434"); NULL
 *   selects the default.
 * @num_ctx: context-window size; <= 0 selects the provider default.
 * @keep_alive_secs: model keep-alive; <= 0 selects the provider
 *   default.
 * @effort: borrowed reasoning-effort hint, or NULL/empty for the
 *   provider default; invalid values are rejected with an error logged.
 *
 * Return: caller-owned LLMProvider released via its destroy() callback,
 * or NULL on invalid effort or allocation failure. Thread-safe; no
 * shared state.
 */
LLMProvider *ollama_provider_create(const char *base_url, int num_ctx,
                                    int keep_alive_secs, const char *effort);

/**
 * ollama_reasoning_effort_valid - validate an effort value for ollama
 * @effort: value to validate; NULL and empty are accepted.
 *
 * Return: 1 when effort is NULL/empty or one of ollama's accepted
 * values; 0 otherwise. Thread-safe; immutable.
 */
int ollama_reasoning_effort_valid(const char *effort);

#ifdef OLLAMA_TEST
/**
 * ollama_test_set_alloc_fail - make the Nth allocation fail here
 * @nth_allocation: 1-based index of the next str_dup/realloc/calloc
 *   call to fail; -1 disables fault injection.
 *
 * Test-only hook. Resets the shared call counter, fails the Nth
 * allocation (only that call), and leaves every other allocation to
 * behave normally. Single-threaded tests only.
 *
 * Return: nothing.
 */
void ollama_test_set_alloc_fail(int nth_allocation);

/**
 * ollama_test_parse_stream_calls_json - run the real tool-call parser
 * @raw: raw JSON payload (a streamed message object).
 *
 * Test-only hook driving the production parse_stream_tool_calls over a
 * payload, so tests and fuzz targets exercise real code (including its
 * keep-old-capacity OOM behavior). Extracts the "message" object like
 * the streaming loop does.
 *
 * Return: the parsed tool-call count, or -1 on JSON parse failure.
 * Thread-safe; pure.
 */
int ollama_test_parse_stream_calls_json(const char *raw);

/**
 * ollama_test_parse_response - parse a complete non-stream response
 * @raw: the raw /api/chat response body.
 *
 * Test-only hook wrapping the production response parser.
 *
 * Return: caller-owned LLMResponse (free with llm_response_free()), or
 * NULL on parse/allocation failure.
 */
LLMResponse *ollama_test_parse_response(const char *raw);
#endif

#endif
