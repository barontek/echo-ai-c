/*
 * routes_chat.h - HTTP handlers for the chat endpoints: blocking POST
 * /api/chat and the EventSource streaming GET /api/stream.
 * Depends on: routes.h.
 */

#ifndef ECHO_ROUTES_CHAT_H
#define ECHO_ROUTES_CHAT_H

#include "routes.h"

/**
 * handle_chat - run one agent turn and return the final response (POST /api/chat)
 * @req: parsed request; borrowed, valid only for this call. req->body is
 *   owned by the server and must not be freed by the handler. Body is
 *   JSON {"message": "..."}.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; reads ctx->agent, ctx->metrics.
 *
 * Gated in-handler via middleware_check_unlock: 401 without a valid
 * X-Unlock-Token. 400 for a missing body, invalid JSON, or a missing
 * message field; 500 "agent not initialized" / "agent returned no
 * response" when the agent is missing or its run yields nothing. The
 * agent loop runs synchronously, so the handler blocks the loop until
 * the turn completes. On success 200 {"content", "has_tools",
 * "thinking"?, "tool_calls"?: [{name, arguments}, ...]}.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety:
 * libuv loop thread only; reads shared ctx state.
 */
void handle_chat(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_sse_stream - stream one agent turn over Server-Sent Events (GET /api/stream)
 * @req: parsed request; borrowed. The token is accepted from the
 *   X-Unlock-Token header or the ?token= query parameter.
 * @client: connection; borrowed; receives the SSE response and is closed
 *   by this handler — the caller must not reuse it afterwards.
 * @ctx: shared server state; borrowed; reads ctx->agent.
 *
 * The unlock check runs both here (header OR query token, for
 * EventSource clients that cannot set headers) and in the route table
 * via unlock_via_query; 401 when both fail. 500 "no agent" when no
 * agent is configured. On success the raw SSE headers are written first,
 * then one "data: {"type":"content","content":...}\n\n" frame per model
 * chunk, and a terminal "done" or "error" frame before client_close().
 * The agent run blocks until the turn completes. OOM while building the
 * header frame responds 500; OOM while allocating the per-connection
 * context (after the 200 headers are out) writes an SSE error frame and
 * closes the client — neither path leaves the connection dangling.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety:
 * libuv loop thread only; reads shared ctx state.
 */
void handle_sse_stream(HTTPRequest *req, Client *client, ServerContext *ctx);

#ifdef ROUTES_CHAT_TEST
/**
 * routes_chat_test_set_alloc_fail - make the Nth allocation fail here
 * @nth_allocation: 1-based index of the next asprintf/calloc call in
 *   routes_chat.c that should fail; -1 disables fault injection.
 *
 * Test-only hook for the handle_sse_stream OOM paths. The call counter
 * resets on every arm, so the index counts from the next routes_chat
 * call onward.
 *
 * Return: void; never fails.
 */
void routes_chat_test_set_alloc_fail(int nth_allocation);
#endif

#endif
