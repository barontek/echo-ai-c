/*
 * routes_session.h - HTTP handlers for session CRUD: list, create, get
 * (with /export and /debug-export variants), delete, update, rename, and
 * import. Depends on: routes.h.
 */

#ifndef ECHO_ROUTES_SESSION_H
#define ECHO_ROUTES_SESSION_H

#include "routes.h"

/**
 * handle_sessions - list all sessions (GET /api/sessions)
 * @req: parsed request; borrowed, valid only for this call.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; reads ctx->sm.
 *
 * Gated in-handler via middleware_check_unlock: 401 without a valid
 * X-Unlock-Token. With session management disabled (ctx->sm == NULL) the
 * response is 200 {"sessions":[],"session_enabled":false}; a failed list
 * also yields 200 {"sessions":[]}. Otherwise 200 with
 * {"sessions":[{id, session_id, title, created_at,
 * title_generation_attempted}, ...]}.
 *
 * Return: void; the HTTP status is the error signal. Allocation failure
 * inside cJSON silently drops fields — the body may come back truncated.
 * Thread-safety: libuv loop thread only; reads shared ctx state.
 */
void handle_sessions(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_create_session - create a new session (POST /api/sessions)
 * @req: parsed request; borrowed. Optional JSON body {"title"} — anything
 *   else is ignored and the default title "Chat Session" is used.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; reads ctx->sm.
 *
 * Gated in-handler via middleware_check_unlock: 401 without a valid
 * X-Unlock-Token. 400 "session manager not available" when session
 * management is disabled; 500 "failed to create session" when the
 * manager rejects the request. On success 200 {"session_id", "title",
 * "created_at"}.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety:
 * libuv loop thread only; reads shared ctx state.
 */
void handle_create_session(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_session_get - fetch one session's full record (GET /api/sessions/<id>)
 * @req: parsed request; borrowed. The session id is parsed out of
 *   req->path; the returned pointer aliases it, valid only for the call.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; reads ctx->sm.
 *
 * Unlock is enforced by the route table (needs_unlock), not in-handler.
 * 400 when session management is disabled or the id is missing from the
 * path. A trailing /export or /debug-export suffix switches to the
 * export payload: 200 with the exported JSON, 404 when the session does
 * not exist. Otherwise 200 {"id", "session_id", "title", "created_at",
 * "messages":[...], "branches":[...]?}; 404 "session not found" for an
 * unknown id. branches is omitted on OOM.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety:
 * libuv loop thread only; reads shared ctx state.
 */
void handle_session_get(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_session_delete - delete a session (DELETE /api/sessions/<id>)
 * @req: parsed request; borrowed. The session id is parsed out of
 *   req->path; the body is ignored.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; reads ctx->sm.
 *
 * Unlock is enforced by the route table (needs_unlock), not in-handler.
 * 400 when session management is disabled or the id is missing; 500
 * "delete failed" when the storage layer errors; 404 "session not found"
 * for an unknown id; 200 {"deleted":true} on success.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety:
 * libuv loop thread only; reads shared ctx state.
 */
void handle_session_delete(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_session_update - rename a session's title (PUT /api/sessions/<id>)
 * @req: parsed request; borrowed. Body is JSON {"title"} (required); the
 *   session id comes from req->path.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; reads and writes ctx->sm.
 *
 * Unlock is enforced by the route table (needs_unlock), not in-handler.
 * 400 for missing id, missing/invalid body, or missing title; 404
 * "session not found" for an unknown id; 500 "failed to save session"
 * when the write fails. On success 200 {"id", "session_id", "title",
 * "created_at"}.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety:
 * libuv loop thread only; reads/writes shared ctx state.
 */
void handle_session_update(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_sessions_rename - rename a session by body id (POST /api/sessions/rename)
 * @req: parsed request; borrowed. Body is JSON {"session_id",
 *   "new_title"}, both required.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; reads and writes ctx->sm.
 *
 * Gated in-handler via middleware_check_unlock: 401 without a valid
 * X-Unlock-Token. 400 when session management is disabled, the body is
 * missing or invalid JSON, or either field is absent; 404 "session not
 * found" for an unknown id; 500 "failed to save session" on write
 * failure. On success 200 {"id", "session_id", "title", "created_at"}.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety:
 * libuv loop thread only; reads/writes shared ctx state.
 */
void handle_sessions_rename(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_session_import - import a session from a JSON payload (POST /api/sessions/import)
 * @req: parsed request; borrowed. The raw body (exported-session JSON) is
 *   handed to the session manager as-is.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; reads and writes ctx->sm.
 *
 * Unlock is enforced by the route table (needs_unlock), not in-handler.
 * 400 when session management is disabled, the body is missing, or the
 * import is rejected (duplicate id or invalid session data). On success
 * 200 {"id", "title"}.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety:
 * libuv loop thread only; reads/writes shared ctx state.
 */
void handle_session_import(HTTPRequest *req, Client *client, ServerContext *ctx);

#endif
