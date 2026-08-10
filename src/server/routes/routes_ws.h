/*
 * routes_ws.h - per-connection WebSocket chat: context creation on the
 * upgrade handshake and auth invalidation on logout.
 * Depends on: routes.h.
 */

#ifndef ECHO_ROUTES_WS_H
#define ECHO_ROUTES_WS_H

#include "routes.h"

/**
 * routes_ws_chat_init - set up a per-connection chat context on a WS upgrade
 * @ws: connection being upgraded; borrowed. On success it owns the new
 *   context (ws->userdata), which is freed when the connection closes.
 * @ctx: shared server state; borrowed. The context keeps a reference for
 *   auth checks and provider config; ctx outlives the connection.
 * @query: query string of the upgrade request (may be NULL or empty);
 *   an optional "session_id=" parameter preloads that session.
 *
 * Mints a per-connection Agent from ctx->agent_cfg (never shares
 * ctx->agent), retains the session manager, and wires the message,
 * close, approval, ask-user, title, and tool callbacks. Emits
 * "session_start" and "ready" frames (plus a "history" frame when a
 * session was preloaded) and flushes messages queued before "ready".
 *
 * Return: void; on OOM the connection is left without chat callbacks —
 * nothing is signaled. Thread-safety: call on the libuv loop thread at
 * handshake time only.
 */
void routes_ws_chat_init(WSClient *ws, ServerContext *ctx, const char *query);

/**
 * routes_ws_invalidate_auth - tear down chat auth on logout
 * @ctx: server context whose active WS chat contexts are invalidated;
 *   NULL is accepted.
 *
 * Called by /api/logout: for every live chat context cancels the active
 * agent run, drops the session-manager reference, and unblocks pending
 * approval/ask_user prompts (approval denied, ask_user times out).
 * Contexts themselves are freed by their connection close callbacks.
 *
 * Return: void; never fails. Thread-safety: call on the libuv loop
 * thread; mutates the ws_chat_contexts list.
 */
void routes_ws_invalidate_auth(ServerContext *ctx);

#endif
