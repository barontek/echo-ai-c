/*
 * routes_openai_auth.h - HTTP handlers for the OpenAI OAuth flow: status
 * polling, starting a browser login, and sign-out/cancel.
 * Depends on: routes.h.
 */

#ifndef ECHO_ROUTES_OPENAI_AUTH_H
#define ECHO_ROUTES_OPENAI_AUTH_H

#include "routes.h"

/**
 * handle_openai_oauth_status - report OpenAI sign-in state (GET /api/auth/openai/status)
 * @req: parsed request; borrowed, valid only for this call. An optional
 *   "login_id=" query parameter restricts the report to one in-progress
 *   login flow.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed. ctx->openai_oauth must be non-NULL,
 *   else 500 "OpenAI OAuth unavailable".
 *
 * Without login_id (or with no query string at all): 200 {"state":
 * pending|signed_in|signed_out, plus account_id/plan_type/error when
 * known}. With login_id: the same report for that login flow; 400 when
 * the query is present but does not carry a valid login_id (id must be
 * 1-128 characters of [A-Za-z0-9_-], no '&'), and 404 when the login is
 * no longer active.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety: runs
 * on the single libuv loop thread; reads shared ctx state only.
 */
void handle_openai_oauth_status(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_openai_oauth_start - begin a browser login (POST /api/auth/openai/start)
 * @req: parsed request; borrowed. The body is ignored.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed. ctx->openai_oauth and ctx->sm must
 *   both be non-NULL (the OAuth store persists through the unlocked
 *   session manager), else 503.
 *
 * On success: 200 {"authorization_url", "login_id"} for the browser flow.
 * When a login is already pending: 409. Any other start failure: 500.
 * If the response cannot be built after a successful start, the just-
 * started login is cancelled (openai_oauth_cancel_login) before the
 * 500 is sent, so no orphaned pending login is left behind.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety: runs
 * on the single libuv loop thread; mutates OAuth flow state in ctx.
 */
void handle_openai_oauth_start(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_openai_oauth_logout - sign out, or cancel one login (POST /api/auth/openai/logout)
 * @req: parsed request; borrowed. An optional JSON body {"login_id"} cancels
 *   that specific login flow instead of signing out wholesale.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed. ctx->openai_oauth must be non-NULL,
 *   else 500 "OpenAI OAuth unavailable".
 *
 * 400 when the body is present but lacks a non-empty string login_id;
 * 500 when the cancel/sign-out call fails; 200 {"signed_out":true}
 * otherwise.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety: runs
 * on the single libuv loop thread; mutates OAuth flow state in ctx.
 */
void handle_openai_oauth_logout(HTTPRequest *req, Client *client, ServerContext *ctx);

#endif
