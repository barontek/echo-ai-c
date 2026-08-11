/*
 * middleware.h - request-gating checks shared by the HTTP and websocket
 * paths: header extraction, constant-time token comparison, unlock-state
 * validation, and rate limiting.
 * Depends on: OpenSSL (constant-time compare), server.h, utils/rate_limiter.h.
 */

#ifndef ECHO_MIDDLEWARE_H
#define ECHO_MIDDLEWARE_H

#include <stddef.h>
#include "server.h"

/**
 * token_equals - constant-time comparison of a token against a string
 * @a: token bytes with an explicit length; NULL compares unequal.
 * @a_len: number of bytes of @a to compare.
 * @b: NUL-terminated token; NULL compares unequal.
 *
 * Uses CRYPTO_memcmp so the comparison time does not reveal how many
 * leading bytes match.
 *
 * Return: 1 if the byte strings are equal, 0 otherwise. Never fails;
 * thread-safe (no shared state).
 */
int token_equals(const char *a, size_t a_len, const char *b);

/**
 * middleware_has_valid_token - check the X-Unlock-Token request header
 * @headers: raw request header block (as stored in HTTPRequest.headers);
 *   NULL is rejected.
 * @token: expected unlock token, NUL-terminated; NULL is rejected.
 *
 * Return: 1 when the header value equals token (constant-time), 0 when
 * the header is absent, args are NULL, or the values differ. Never
 * fails; thread-safe (no shared state).
 */
int middleware_has_valid_token(const char *headers, const char *token);

/**
 * middleware_has_valid_ws_token - check the websocket upgrade token
 * @headers: raw request header block; NULL is rejected.
 * @token: expected unlock token, NUL-terminated; NULL is rejected.
 *
 * The Sec-WebSocket-Protocol header is split on commas and any item of
 * the form "echo-ai-token-<token>" is accepted (constant-time compare).
 * The token rides in the subprotocol because browser WebSocket APIs
 * cannot set custom headers.
 *
 * Return: 1 if a matching item exists, 0 otherwise. Never fails;
 * thread-safe (no shared state).
 */
int middleware_has_valid_ws_token(const char *headers, const char *token);

/**
 * middleware_check_unlock - gate an unlock-protected HTTP route
 * @req: request whose headers are checked; must be non-NULL.
 * @ctx: server context; must be non-NULL.
 *
 * Open only when the server is STATE_UNLOCKED. An unlock_token of "noop"
 * (session-management-disabled mode) opens every gated route; otherwise
 * the token is compared against the X-Unlock-Token header.
 *
 * Return: 1 if the request may pass, 0 if the caller should reject it
 * with 401. Never fails; safe only on the libuv loop thread (reads
 * unsynchronized ServerContext state and a rate limiter that requires
 * caller serialization).
 */
int middleware_check_unlock(HTTPRequest *req, ServerContext *ctx);

/**
 * middleware_check_unlock_query - gate a route via a ?token= query param
 * @req: request whose query string is checked; NULL is rejected.
 * @ctx: server context; NULL is rejected.
 *
 * Same semantics as middleware_check_unlock(), but the token is taken
 * from the token= query parameter — the transport SSE clients must use
 * because EventSource cannot set headers. The value is compared raw and
 * is NOT URL-decoded, so clients must send it unescaped.
 *
 * Return: 1 if the request may pass, 0 otherwise. Never fails; safe
 * only on the libuv loop thread (same ServerContext reads as
 * middleware_check_unlock).
 */
int middleware_check_unlock_query(HTTPRequest *req, ServerContext *ctx);

/**
 * middleware_check_rate_limit - apply the rate limiter to a request
 * @req: request whose client IP is rated; must be non-NULL.
 * @ctx: context holding the limiter; a NULL ctx->rate_limiter admits
 *   every request.
 *
 * Return: 1 when the request is allowed (or no limiter is configured),
 * 0 when it exceeds the limit and the caller should reply 429. Never
 * fails; safe only on the libuv loop thread (the RateLimiter requires
 * caller serialization per rate_limiter.h).
 */
int middleware_check_rate_limit(HTTPRequest *req, ServerContext *ctx);

/**
 * middleware_add_cors - answer a CORS preflight with 204
 * @client: connection to answer; NULL or a websocket client is ignored.
 * @ctx: unused — every server_response() already carries the CORS
 *   headers.
 *
 * Return: void; never fails. Single-threaded: call on the loop thread.
 */
void middleware_add_cors(Client *client, ServerContext *ctx);

#endif
