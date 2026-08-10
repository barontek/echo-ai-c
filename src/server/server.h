/*
 * server.h - libuv HTTP server core: connection lifecycle, request
 * parsing, static file serving, and the response API routes use.
 * Depends on: libuv, routes/, websocket.h, middleware.h, agent/,
 * session/, tools/, safety/, utils/.
 */

#ifndef ECHO_SERVER_H
#define ECHO_SERVER_H

#include <uv.h>
#include "../config/config.h"
#include "../agent/agent.h"
#include "../session/session_manager.h"
#include "../tools/registry.h"
#include "../safety/safety.h"
#include "../utils/rate_limiter.h"
#include "../utils/metrics.h"
#include "../change_tracker/change_tracker.h"

typedef enum {
    STATE_LOCKED,
    STATE_SETUP,
    STATE_UNLOCKED
} ServerState;

/* Populated and torn down by main.c; server_start() fills loop and takes
 * the connection lifecycle over from there. String pointers other than
 * config_path are borrowed: conf strings and the shared agent outlive the
 * server. */
struct WSChatCtx;

typedef struct {
    ServerState state;
    Agent *agent;
    /* D2: per-connection Agent instances for the WS path are minted from
     * this config in routes_ws_chat_init and destroyed in ws_chat_on_close.
     * The shared `agent` field remains used by the REST /chat path only
     * (single-threaded per libuv request). Stored by value so its inner
     * string pointers alias Conf strings (which outlive the server). */
    AgentConfig agent_cfg;
    OpenAIOAuth *openai_oauth;
    SessionManager *sm;
    SafetyConfig *safety;
    char *config_path;
    Conf *conf;
    char *unlock_token;
    unsigned long auth_generation;
    uv_loop_t *loop;
    int port;
    RateLimiter *rate_limiter;
    Metrics *metrics;
    ChangeTracker *change_tracker;
    struct WSChatCtx *ws_chat_contexts;
} ServerContext;

typedef struct Client Client;

/* req->body aliases the Client-owned body buffer (valid for the duration
 * of the handler call); all other fields are fixed-size copies. */
typedef struct {
    char method[16];
    char path[1024];
    char query[512];
    char headers[4096];
    char *body;
    size_t body_len;
    struct sockaddr_storage addr;
    char ip[64];
    Client *client;
    char req_id[32];
} HTTPRequest;

typedef void (*route_handler)(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * server_start - run the HTTP server until it is stopped
 * @ctx: caller-owned context with state, port, rate_limiter, metrics and
 *   change_tracker populated; loop is set by this function and owned by
 *   the server for the duration of the run.
 *
 * Ignores SIGPIPE, binds 0.0.0.0:ctx->port and runs the libuv event loop,
 * returning only after server_stop() or a fatal loop exit. Must be called
 * from the thread that then runs the loop.
 *
 * Return: 0 on normal loop exit, -1 if binding or listening fails (the
 * error is logged and the listen handle freed). ctx stays caller-owned.
 */
int server_start(ServerContext *ctx);

/**
 * server_stop - stop the event loop running in server_start()
 * @ctx: context passed to server_start(); NULL is accepted. No-op if the
 *   loop was never started (ctx->loop is NULL).
 *
 * Return: void; never fails. Safe to call from another thread (uv_stop is
 * thread-safe); server_start() then returns 0.
 */
void server_stop(ServerContext *ctx);

/**
 * server_response - send an HTTP response, then close the connection
 * @client: connection to respond on; NULL or a websocket-upgraded client
 *   is ignored.
 * @status: HTTP status code; 204 sends no body and no Content-Length.
 * @content_type: MIME type, or NULL for text/plain. Borrowed for the
 *   duration of the call.
 * @body: response body, borrowed and copied into the write buffer; NULL
 *   sends an empty body.
 *
 * The client is closed once the write completes, so the caller must not
 * touch client afterwards. Allocation failures are logged with request
 * context and reported through the return; the peer just sees a closed
 * connection.
 *
 * Return: 0 on success, -1 when the response could not be built or
 * queued (alloc failure, or a NULL/websocket client). Callers have no
 * recovery path for a broken peer, so the return may be ignored.
 * Single-threaded: call on the loop thread only.
 */
int server_response(Client *client, int status, const char *content_type, const char *body);

/**
 * server_response_json - send a JSON HTTP response and close the connection
 * @client: connection to respond on; see server_response().
 * @status: HTTP status code.
 * @json: pre-serialized JSON payload, NUL-terminated and borrowed for the
 *   duration of the call.
 *
 * Thin wrapper over server_response() with Content-Type
 * application/json; same close-on-write semantics.
 *
 * Return: 0 on success, -1 as documented for server_response().
 */
int server_response_json(Client *client, int status, const char *json);

/**
 * server_response_error - send an {"error": msg} JSON response
 * @client: connection to respond on.
 * @status: HTTP status code (typically 4xx/5xx).
 * @msg: human-readable error message; NUL-terminated and borrowed for the
 *   duration of the call.
 *
 * Return: 0 on success, -1 as documented for server_response().
 */
int server_response_error(Client *client, int status, const char *msg);

/**
 * client_close - close a connection and free everything it owns
 * @client: connection to close; NULL accepted. Idempotent: a client that
 *   is already closed or closing is left alone.
 *
 * The Client struct, its buffers, and the ws_private pointer attached
 * with client_set_ws_private() are released by the close callback — the
 * pointer is freed with free(), so it must have been heap-allocated or
 * NULL. Do not touch client after the call.
 *
 * Return: void; never fails. Call on the loop thread.
 */
void client_close(Client *client);

/**
 * client_set_ws_private - attach a per-connection private pointer
 * @client: connection to attach to; must be non-NULL.
 * @priv: pointer the client owns from now on; released with free() when
 *   the connection closes (see client_close()). Typically the WSClient
 *   built by ws_do_handshake().
 *
 * The HTTP layer only stores the pointer and never dereferences it.
 *
 * Return: void; never fails.
 */
void client_set_ws_private(Client *client, void *priv);

#endif
