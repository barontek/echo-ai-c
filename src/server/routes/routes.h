/*
 * routes.h - HTTP route table and shared route helpers (message
 * serialization, path matching, SSE writes) for the REST API.
 * Depends on: server.h, cJSON.
 */

#ifndef ECHO_ROUTES_H
#define ECHO_ROUTES_H

#include <cjson/cJSON.h>
#include "../server.h"

typedef struct WSClient WSClient;

/* One entry of the static dispatch table. path is matched exactly when
 * is_prefix is 0, as a path prefix when 1. needs_unlock gates the route in
 * server.c's dispatcher before the handler runs; unlock_via_query makes that
 * check read the token from the query string instead of the X-Unlock-Token
 * header (used by EventSource, which cannot set headers). */
typedef struct {
    const char *method;
    const char *path;
    int is_prefix;
    int needs_unlock;
    int unlock_via_query;
    route_handler handler;
} Route;

/* Owned by this module: the dispatch table and its length. Read-only for
 * everyone else; valid for the lifetime of the server. */
extern const Route routes[];
extern const int routes_count;

/**
 * route_match - test a request method and path against one route entry
 * @method: HTTP method ("GET", "POST", ...).
 * @path: request path.
 * @r: route entry to test; must be non-NULL.
 *
 * Compares method exactly and path either exactly (is_prefix == 0) or as a
 * string prefix (is_prefix == 1). Prefix matches are not segment-aware:
 * "/api/sessions/" also matches "/api/sessions/1/x".
 *
 * Return: 1 on match, 0 otherwise. Never fails; thread-safe (reads only).
 */
int route_match(const char *method, const char *path, const Route *r);

/**
 * ws_add_message_to_json - serialize one message into a JSON object
 * @m: caller-owned cJSON object to populate; must be non-NULL and not yet
 *   part of a larger tree — fields are added directly to it.
 * @msg: message to serialize; borrowed and read-only, valid for the call.
 *   Must be non-NULL; the caller keeps ownership and frees it.
 *
 * Emits role, content, and the optional identity fields (id, parent_id,
 * fork_group_id), thinking, tool_name, tool_call_id, error_category, and a
 * tool_calls array with has_tools only when present. NULL/empty strings are
 * coalesced to "unknown"/"".
 *
 * Return: void. Allocation failure inside cJSON silently drops fields —
 * callers cannot distinguish a truncated object. Thread-safe (reads only).
 */
void ws_add_message_to_json(cJSON *m, const Message *msg);

/**
 * server_sse_write - queue a Server-Sent-Events frame on a client
 * @client: connection to write to; borrowed. Non-WS connections only.
 * @data: frame text (typically "data: ...\n\n"); borrowed, copied
 *   synchronously, so the caller may free it after return.
 *
 * The copy is written asynchronously on the libuv loop; this call never
 * blocks. Failure (OOM, or client already upgraded to WebSocket) is silent.
 *
 * Return: void; failures are logged nowhere and must be treated as best
 * effort. Thread-safety: libuv loop thread only, matching its callers.
 */
void server_sse_write(Client *client, const char *data);

#endif
