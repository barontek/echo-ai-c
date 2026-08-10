/*
 * openai_compatible.h - OpenAI-compatible chat client for servers exposing
 * the /v1/chat/completions API (LM Studio, vLLM, llama.cpp server, ...).
 * Depends on: provider.h.
 */

#ifndef ECHO_OPENAI_COMPATIBLE_H
#define ECHO_OPENAI_COMPATIBLE_H

#include <stddef.h>

#include "provider.h"

/**
 * openai_compatible_provider_create - construct an OpenAI-compatible provider
 * @base_url: endpoint, e.g. http://localhost:1234, or NULL to use the
 *   default (https://api.openai.com). Borrowed for the duration of the call;
 *   the provider keeps its own copy.
 * @api_token: optional Bearer token sent as an Authorization header, or
 *   NULL/empty for no auth. Borrowed for the duration of the call; the
 *   provider keeps its own copy.
 * @effort: borrowed reasoning-effort hint, or NULL/empty for the API
 *   default; otherwise one of "low", "medium", "high", "max", "none".
 *   Any other value is rejected.
 *
 * When effort is set, "reasoning_effort" is embedded in every
 * chat-completions request body. Rejects invalid effort with NULL returned
 * and an error logged.
 *
 * Return: caller-owned LLMProvider, or NULL on invalid effort or allocation
 * failure. The caller must release it via its destroy() callback. Safe to
 * call concurrently: the provider keeps no mutable shared state and each
 * request uses its own CURL handle.
 */
LLMProvider *openai_compatible_provider_create(const char *base_url,
                                               const char *api_token,
                                               const char *effort);

/**
 * openai_compatible_reasoning_effort_valid - check a reasoning-effort value
 * @effort: value to validate; NULL and empty are accepted.
 *
 * Returns 1 when effort is NULL, empty, or one of "low", "medium", "high",
 * "max", "none"; 0 otherwise. "xhigh" is intentionally absent because
 * OpenAI-compatible endpoints are not required to support it.
 *
 * Return: 1 if accepted, 0 otherwise. Never fails; thread-safe.
 */
int openai_compatible_reasoning_effort_valid(const char *effort);

#ifdef OPENAI_COMPATIBLE_TEST
/**
 * openai_compatible_test_parse_stream - parse a whole SSE payload as one buffer
 * @input: complete SSE response text ("data: ..." lines, optionally
 *   terminated by "data: [DONE]").
 * @on_chunk: callback fired once per content delta with the raw text
 *   (including <think> tags), or NULL to suppress. Called synchronously.
 * @userdata: opaque pointer forwarded to on_chunk unchanged.
 *
 * Test-only hook for the buffered streaming path; mirrors what the live
 * streaming request does once the response is fully received.
 *
 * Return: caller-owned LLMResponse (aggregated content and tool calls), or
 * NULL on allocation or parse failure. Free with llm_response_free().
 * Thread-safe; no shared state.
 */
LLMResponse *openai_compatible_test_parse_stream(
    const char *input, void (*on_chunk)(const char *, void *), void *userdata);

/**
 * openai_compatible_test_stream_fragments - parse SSE split across fragments
 * @fragments: array of byte fragments that together form the SSE payload.
 * @lengths: per-fragment byte lengths, or NULL to strlen() each fragment.
 * @count: number of fragments (>= 1).
 * @on_chunk: callback fired once per content delta with the raw text
 *   (including <think> tags), or NULL to suppress. Called synchronously.
 * @userdata: opaque pointer forwarded to on_chunk unchanged.
 *
 * Test-only hook exercising the partial-line buffering path, i.e. the same
 * delivery shape curl produces when the network splits lines across reads.
 *
 * Return: caller-owned LLMResponse (aggregated content and tool calls), or
 * NULL on invalid arguments, allocation, or parse failure. Free with
 * llm_response_free(). Thread-safe; no shared state.
 */
LLMResponse *openai_compatible_test_stream_fragments(
    const char **fragments, size_t *lengths, int count,
    void (*on_chunk)(const char *, void *), void *userdata);

/**
 * openai_compatible_test_build_url - resolve a chat-completions endpoint URL
 * @base_url: endpoint; a path already ending in /chat/completions is used
 *   as-is, a /v1 suffix gets /chat/completions appended, anything else gets
 *   /v1/chat/completions appended.
 *
 * Test-only hook mirroring the provider's URL resolution. NULL is accepted
 * only via the provider create default, not here.
 *
 * Return: caller-owned null-terminated URL string, or NULL on allocation
 * failure. Free with free(). Thread-safe; no shared state.
 */
char *openai_compatible_test_build_url(const char *base_url);

/**
 * openai_compatible_test_build_body - build a chat-completions request body
 * @model: model name string.
 * @msgs_json: pre-serialized "messages" array JSON.
 * @stream: 1 for a streaming request body, 0 otherwise.
 * @temperature: sampling temperature.
 * @tools_json: pre-serialized "tools" array JSON, or NULL/empty for none.
 * @json_schema: pre-serialized JSON schema, or NULL/empty for none.
 * @force_json_format: 1 to force the json_object response format (used by
 *   extract_structured), 0 otherwise.
 * @effort: reasoning-effort hint, or NULL/empty for the API default.
 *
 * Test-only hook building the body exactly as the provider does. tools_json
 * and json_schema are mutually exclusive; json_schema takes precedence and
 * forces a non-streaming json_schema response_format.
 *
 * Return: caller-owned JSON string, or NULL on invalid effort or allocation
 * failure. Free with free(). Thread-safe; no shared state.
 */
char *openai_compatible_test_build_body(const char *model, const char *msgs_json,
                                        int stream, double temperature,
                                        const char *tools_json,
                                        const char *json_schema,
                                        int force_json_format,
                                        const char *effort);
#endif

#endif
