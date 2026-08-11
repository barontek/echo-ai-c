/*
 * openai_response.h - Codex response parsing and HTTP plumbing
 * contracts: buffered-response extraction, credential/header
 * management, 401 recovery, and model-catalog parsing helpers.
 * Depends on: openai_internal.h.
 */

#ifndef ECHO_OPENAI_RESPONSE_H
#define ECHO_OPENAI_RESPONSE_H

#include <curl/curl.h>

#include "openai_internal.h"

/**
 * openai_credentials_clear - free a Credentials pair
 * @credentials: pair to release; NULL is a no-op.
 *
 * The token is zeroed before free; account is freed plainly.
 *
 * Return: void.
 */
void openai_credentials_clear(Credentials *credentials);

/**
 * request_setup - configure a curl handle for a Codex POST
 * @curl: handle to configure.
 * @body: request body; validated for size.
 * @timeout: timeout in seconds (> 0).
 * @credentials: token/account used to build the Authorization headers.
 * @headers_out: receives the caller-owned header list on success.
 *
 * Return: 0 on success, -1 on invalid arguments, header build failure,
 * or curl setopt failure (headers freed, *headers_out NULL).
 */
int request_setup(CURL *curl, const char *body, int timeout,
                  const Credentials *credentials,
                  struct curl_slist **headers_out);

/**
 * headers_free - free a header list, cleansing the Authorization line
 * @headers: list to free; NULL is a no-op.
 *
 * The Bearer token is zeroed before free.
 *
 * Return: void.
 */
void headers_free(struct curl_slist *headers);

/**
 * credentials_get - fetch a fresh access token from the OAuth manager
 * @auth: borrowed OAuth manager; must be signed in.
 * @credentials: receives a caller-owned token/account (credentials_clear()).
 *
 * Return: 0 on success, -1 on failure with the pair cleared.
 */
int credentials_get(OpenAIOAuth *auth, Credentials *credentials);

/**
 * credentials_refresh_401 - refresh credentials after a 401
 * @auth: borrowed OAuth manager.
 * @credentials: pair whose token was rejected; replaced in place on
 *   success.
 *
 * Return: 0 on success, -1 on refresh failure (pair left unchanged).
 */
int credentials_refresh_401(OpenAIOAuth *auth, Credentials *credentials);

/**
 * add_response_tool_call - append a tool call to a response
 * @response: response to grow; tool_calls_count must be in range.
 * @id: call id; NULL becomes "".
 * @name: tool name; NULL becomes "".
 * @arguments: arguments JSON; NULL becomes "".
 * @index_out: optional index of the appended call.
 *
 * Return: 0 on success, -1 on invalid response, overflow, or allocation
 * failure (no partial state committed).
 */
int add_response_tool_call(LLMResponse *response, const char *id,
                           const char *name, const char *arguments,
                           int *index_out);

/**
 * response_status_ok - check a response envelope reports success
 * @root: parsed response object.
 *
 * Return: 1 when there is no error field (or it is null) and the status
 * is absent or "completed"; 0 otherwise.
 */
int response_status_ok(const cJSON *root);

/**
 * log_http_error - log a non-2xx Codex response with safe fields
 * @operation: operation name for the log context.
 * @status: HTTP status code.
 * @body: response body, parsed for error type/code; may be NULL.
 *
 * Return: void.
 */
void log_http_error(const char *operation, long status, const char *body);

/**
 * parse_models_response - parse a Codex models-catalog response
 * @raw: catalog response JSON body.
 * @models_out: receives a caller-owned array of list-visible slugs, or
 *   NULL when the catalog is empty. Free with openai_models_free().
 * @count_out: receives the number of slugs (0 when empty).
 *
 * Return: 0 on success, -1 on invalid arguments or parse failure with
 * the outputs left NULL/0.
 */
int parse_models_response(const char *raw, char ***models_out,
                          size_t *count_out);

#ifdef OPENAI_TEST
/**
 * parse_response - parse a complete non-streaming Codex response
 * @raw: Responses API JSON document.
 *
 * Test-only entry point behind openai_test_parse_response_alloc();
 * extracts content, reasoning summary into <think> blocks, and
 * function calls.
 *
 * Return: caller-owned LLMResponse, or NULL on parse/validation failure.
 */
LLMResponse *parse_response(const char *raw);
#endif

#endif /* ECHO_OPENAI_RESPONSE_H */
