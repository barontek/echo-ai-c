/*
 * routes_general.h - HTTP handlers for the non-auth misc endpoints:
 * status/health probes, agent config, model and provider discovery,
 * metrics, and the undo/redo change-tracker endpoints.
 * Depends on: routes.h.
 */

#ifndef ECHO_ROUTES_GENERAL_H
#define ECHO_ROUTES_GENERAL_H

#include "routes.h"

/**
 * handle_status - report lock/setup/session state (GET /api/status)
 * @req: parsed request; borrowed; headers are checked for a valid token.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; reads ctx->state, ctx->sm,
 *   ctx->unlock_token.
 *
 * Not unlock-gated. locked=true when the server is STATE_LOCKED, or when
 * STATE_UNLOCKED with a real token and the request carries an invalid or
 * missing one; needs_setup=true only in STATE_SETUP. An unlock_token of
 * "noop" (session-management-disabled mode) never reports locked. Always
 * 200 {"locked", "needs_setup", "session_enabled"}.
 *
 * Return: void; never fails, but allocation failure inside cJSON
 * silently drops fields. Thread-safety: libuv loop thread only; reads
 * shared ctx state.
 */
void handle_status(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_health - liveness probe (GET /api/health)
 * @req: ignored.
 * @client: connection; borrowed; receives the response.
 * @ctx: ignored.
 *
 * Always 200 {"status":"ok"}.
 *
 * Return: void; never fails; thread-safe.
 */
void handle_health(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_config - report the active agent configuration (GET /api/config)
 * @req: ignored.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; reads ctx->agent, ctx->sm.
 *
 * Not unlock-gated. 200 {"config": {"provider", "model", "temperature",
 * "max_iterations", "session_enabled"}} from the shared agent when
 * present, else hard-coded defaults (ollama / "" / 0.7 / 50). 500 "oom"
 * when the response cannot be serialized.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety:
 * libuv loop thread only; reads shared ctx state.
 */
void handle_config(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_metrics - render Prometheus-format metrics (GET /api/metrics)
 * @req: ignored.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; reads ctx->metrics.
 *
 * Not unlock-gated. 500 "metrics not available" when the metrics
 * collector is not configured; 500 "oom" on render failure; otherwise
 * 200 text/plain with the rendered metrics.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety:
 * libuv loop thread only; reads shared ctx state.
 */
void handle_metrics(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_undo - revert the last file change (POST /api/undo)
 * @req: parsed request; borrowed; the body is ignored.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; mutates ctx->change_tracker.
 *
 * Unlock-gated by the route table (needs_unlock), not in-handler. 400
 * "change tracker not available" when no tracker is configured. Nothing
 * to undo is not an error: 200 {"undo":false,"reason":"nothing to
 * undo"}. Otherwise 200 {"undo":true,"bytes_restored":N} (falls back to
 * {"undo":true} on OOM).
 *
 * Return: void; the HTTP status is the error signal. Thread-safety:
 * libuv loop thread only; mutates the shared change tracker.
 */
void handle_undo(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_redo - reapply the last undone change (POST /api/redo)
 * @req: parsed request; borrowed; the body is ignored.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; mutates ctx->change_tracker.
 *
 * Unlock-gated by the route table (needs_unlock), not in-handler. 400
 * "change tracker not available" when no tracker is configured. Nothing
 * to redo is not an error: 200 {"redo":false,"reason":"nothing to
 * redo"}. Otherwise 200 {"redo":true,"bytes_written":N} (falls back to
 * {"redo":true} on OOM).
 *
 * Return: void; the HTTP status is the error signal. Thread-safety:
 * libuv loop thread only; mutates the shared change tracker.
 */
void handle_redo(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_health_detailed - report detailed server health (GET /api/health/detailed)
 * @req: ignored.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; reads ctx->sm, ctx->state.
 *
 * Not unlock-gated. Always 200 {"session_enabled", "session_count",
 * "state"} — session_count is omitted when session management is
 * disabled. Allocation failure inside cJSON silently drops fields.
 *
 * Return: void; never fails. Thread-safety: libuv loop thread only;
 * reads shared ctx state.
 */
void handle_health_detailed(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_models - list available models for a provider (GET /api/models)
 * @req: parsed request; borrowed. The provider comes from the
 *   ?provider= query parameter (default "ollama"); "lm_studio" is
 *   translated to "openai_compatible".
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: shared server state; borrowed; reads ctx->openai_oauth,
 *   ctx->conf.
 *
 * Not unlock-gated. For "openai" the remote account is queried when
 * signed in; signed-out, denied, or empty results yield an empty list,
 * and discovery failures fall back to a hard-coded catalog (gpt-5.x).
 * "openai_compatible"/"opencode_zen"/"ollama" fetch the model list from
 * the configured base_url with a 5 s timeout; a curl failure, timeout,
 * or missing list yields 200 with an empty list. Any other provider
 * (e.g. "anthropic") returns 200 with an empty list. OOM inside cJSON
 * yields 500.
 *
 * Return: void; the HTTP status is the error signal. Thread-safety:
 * libuv loop thread only; makes a blocking HTTP request to the model
 * server; reads shared ctx state.
 */
void handle_models(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * handle_providers - list supported providers and effort options (GET /api/providers)
 * @req: ignored.
 * @client: connection; borrowed; receives the HTTP response.
 * @ctx: ignored.
 *
 * Not unlock-gated. 200 {"providers":[...], "effort_supported":[...],
 * "effort_options":{provider: [effort values]}} from the provider
 * registry; falls back to {"providers":[]} on OOM.
 *
 * Return: void; never fails. Thread-safe: reads the static provider
 * registry only.
 */
void handle_providers(HTTPRequest *req, Client *client, ServerContext *ctx);

#endif
