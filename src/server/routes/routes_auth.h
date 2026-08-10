/*
 * routes_auth.h - HTTP handlers for server authentication: initial setup,
 * unlock, logout, and password change. Owns the unlock-token lifecycle.
 * Depends on: routes.h.
 */

#ifndef ECHO_ROUTES_AUTH_H
#define ECHO_ROUTES_AUTH_H

#include "routes.h"

/**
 * handle_setup - first-run password configuration (POST /api/setup)
 * @req: parsed request; borrowed, valid only for this call. req->body is
 *   owned by the server and must not be freed by the handler.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed and MUTATED on success.
 *
 * Only valid while ctx->state == STATE_SETUP; otherwise 400. Body is JSON
 * with a "password" of at least 4 characters. On success creates the
 * session manager under ~/.config/echo-ai, mints an unlock token
 * ("tok_" + 32 random hex bytes), and flips ctx to STATE_UNLOCKED: sets
 * ctx->sm, ctx->unlock_token (freeing the old one), bumps
 * ctx->auth_generation, and re-registers the manager with the OAuth
 * store, tool registry, and shared agent.
 *
 * Errors: 400 for non-SETUP state, missing body, invalid JSON, or a too
 * short password; 500 for OOM, missing HOME, manager init failure, or
 * token generation failure (manager and token are freed on these paths).
 *
 * Return: void; the HTTP status is the error signal. Thread-safety: runs
 * on the single libuv loop thread; mutates shared ctx state, so never
 * call concurrently.
 */
void handle_setup(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_unlock - unlock the server with a password (POST /api/unlock)
 * @req: parsed request; borrowed, valid only for this call. req->body is
 *   owned by the server and must not be freed by the handler.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed and MUTATED on success.
 *
 * When already unlocked and the request carries a valid token the call is
 * idempotent: 200 with the existing token, no re-verification. Otherwise
 * the password is verified by opening a session manager against the
 * stored key material (AUTH_FAILED -> 401 and an unlock-failure record in
 * the rate limiter); success installs the manager into ctx exactly like
 * handle_setup. If the server was already unlocked the freshly verified
 * manager is discarded and the existing token kept.
 *
 * Rate-limited to 5 attempts per 20 s per IP (429 beyond that).
 *
 * Errors: 400 when not in LOCKED/UNLOCKED state or missing password;
 * 401 wrong password; 500 on OOM, missing HOME, or unavailable storage.
 * Password buffers are zeroed before being freed.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety: runs
 * on the single libuv loop thread; mutates shared ctx state, so never
 * call concurrently.
 */
void handle_unlock(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_logout - lock the server and release all unlocked state (POST /api/logout)
 * @req: parsed request; borrowed. The body is ignored.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed and MUTATED: on success frees
 *   ctx->unlock_token and ctx->sm (both set NULL), detaches the OAuth
 *   credential store, invalidates WS chat auth, and flips state to
 *   STATE_LOCKED with auth_generation bumped.
 *
 * Requires a valid X-Unlock-Token header; 401 otherwise. 500 if locking
 * the OAuth credential storage fails (state is left untouched in that
 * case, since the failure happens before any teardown).
 *
 * Return: void; the HTTP status is the error signal. Thread-safety: runs
 * on the single libuv loop thread; mutates shared ctx state, so never
 * call concurrently.
 */
void handle_logout(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_change_password - change the server password (POST /api/change-password)
 * @req: parsed request; borrowed. Body is JSON with "current_password",
 *   "new_password" (at least 8 characters), and optional "confirm" which,
 *   when present, must equal new_password.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; ctx->sm must be non-NULL (the
 *   manager whose key material is re-encrypted), else 400.
 *
 * The current password is proven by opening a throwaway session manager
 * with it (401 + rate-limiter failure record when wrong), then
 * migration_change_password() re-encrypts the vault. Shared the same
 * rate-limit bucket as /api/unlock: 5 attempts per 20 s per IP (429).
 *
 * Errors: 400 missing/invalid JSON, missing fields, or short new
 * password; 500 on OOM, missing HOME, or migration failure — rc == -2
 * (password changed but activation incomplete) is reported distinctly.
 * Password buffers are zeroed before being freed.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety: runs
 * on the single libuv loop thread; reads and writes shared ctx state, so
 * never call concurrently.
 */
void handle_change_password(HTTPRequest *req, Client *client, ServerContext *ctx);

#endif
