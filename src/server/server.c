/*
 * server.c - libuv HTTP server core: connection accept, request dispatch,
 * response helpers, and lifecycle. HTTP parsing lives in http_parse.c,
 * static file serving in serve_static.c.
 * Depends on: libuv, cJSON, routes/, websocket.h, middleware.h,
 * utils/ (logging, string_utils, rate_limiter).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <cjson/cJSON.h>

#include "server.h"
#include "server_internal.h"
#include "serve_static.h"
#include "http_parse.h"
#include "routes/routes.h"
#include "websocket.h"
#include "middleware.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"
#include "../utils/rate_limiter.h"

#ifdef SERVER_TEST
/* Test seams for tests/server/test_server.c: uv_write is captured by the
 * test instead of hitting a real loop, uv_is_closing is stubbed (the test
 * client is not a real uv handle), and every free in this TU is counted so
 * the tests can assert response buffers are actually released. The shim
 * functions must be defined before these #defines so their own bodies call
 * the real functions. */
static int server_test_free_count = 0;
static void server_test_free(void *p)
{
    server_test_free_count++;
    free(p);
}
int server_test_uv_write(uv_write_t *req, uv_stream_t *stream,
                         const uv_buf_t bufs[], unsigned int nbufs,
                         uv_write_cb cb);
int server_test_uv_is_closing(const uv_handle_t *handle);
#define uv_write server_test_uv_write
#define uv_is_closing server_test_uv_is_closing
#define free server_test_free
#endif


void client_set_ws_private(Client *client, void *priv)
{
    client->ws_private = priv;
}

static void client_close_cb(uv_handle_t *handle)
{
    Client *client = (Client *)handle->data;
    if (client)
    {
        free(client->buf);
        free(client->body);
        free(client->ws_private);
        memset(client, 0, sizeof(Client));
        free(client);
    }
}

void client_close(Client *client)
{
    if (!client || client->closed) return;
    client->closed = 1;
    if (!uv_is_closing((uv_handle_t *)&client->handle))
        uv_close((uv_handle_t *)&client->handle, client_close_cb);
    else
        client_close_cb((uv_handle_t *)&client->handle);
}

static void alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf)
{
    (void)handle;
    buf->base = malloc(size);
    buf->len = size;
}

/* Ownership packet for async HTTP writes: the response buffer and the
 * uv_write request must both outlive the write, and the completion
 * callback still needs the client (to close HTTP connections). Grouping
 * them keeps the write path to a single allocation failure point and lets
 * write_done release everything in one place. */
typedef struct {
    Client *client;
    char *buf;
} WriteCtx;

static void write_done(uv_write_t *req, int status)
{
    (void)status;
    WriteCtx *wctx = (WriteCtx *)req->data;
    Client *client = wctx->client;
    free(wctx->buf);
    free(wctx);
    free(req);
    if (client && !client->is_ws)
        client_close(client);
}

static void sse_write_done(uv_write_t *req, int status)
{
    (void)status;
    WriteCtx *wctx = (WriteCtx *)req->data;
    free(wctx->buf);
    free(wctx);
    free(req);
}

/* SSE frame writer for streaming routes: data is duplicated into a
 * self-freeing uv_write request, so the caller can hand back stack
 * buffers. Declared in routes/routes.h; HTTP only — websocket clients
 * are ignored. Failures (allocation, queue full) drop the frame silently.
 * Non-atomic per frame; the loop serializes writes per handle. */
int server_sse_write(Client *client, const char *data)
{
    if (!client || client->is_ws) return -1;
    char *copy = str_dup(data);
    if (!copy)
    {
        log_error("server_sse_write: OOM duplicating frame", NULL);
        return -1;
    }
    uv_buf_t buf = {.base = copy, .len = strlen(copy)};
    uv_write_t *req = malloc(sizeof(uv_write_t));
    WriteCtx *wctx = malloc(sizeof(WriteCtx));
    if (!req || !wctx)
    {
        log_error("server_sse_write: OOM allocating write request", NULL);
        free(wctx);
        free(req);
        free(copy);
        return -1;
    }
    wctx->client = client;
    wctx->buf = copy;
    req->data = wctx;
    if (uv_write(req, (uv_stream_t *)&client->handle, &buf, 1, sse_write_done) != 0)
    {
        /* The request was never queued, so no completion callback will
         * ever run — release the whole packet here instead of leaking. */
        log_error("server_sse_write: uv_write failed", NULL);
        free(wctx->buf);
        free(wctx);
        free(req);
        return -1;
    }
    return 0;
}

int server_response(Client *client, int status, const char *content_type, const char *body)
{
    if (!client || client->is_ws) return -1;

    client->response_status = status;
    char status_buf[16];
    snprintf(status_buf, sizeof(status_buf), "%d", status);
    log_info("response", "status", status_buf,
             "req_id", client->req_id, "path", client->path, NULL);

    char *resp = NULL;
    size_t body_len = body ? strlen(body) : 0;

    const char *status_str = "OK";
    if (status == 404) status_str = "Not Found";
    else if (status == 401) status_str = "Unauthorized";
    else if (status == 400) status_str = "Bad Request";
    else if (status == 413) status_str = "Payload Too Large";
    else if (status == 500) status_str = "Internal Server Error";
    else if (status == 204) status_str = "No Content";

    const char *ct = content_type ? content_type : "text/plain; charset=utf-8";

    if (status == 204)
    {
        if (asprintf(&resp,
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, X-Unlock-Token\r\n"
            "\r\n") < 0)
        {
            log_error("server_response: OOM building 204 response",
                      "req_id", client->req_id, NULL);
            return -1;
        }
    }
    else
    {
        if (asprintf(&resp,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, X-Unlock-Token\r\n"
            "\r\n"
            "%s",
            status, status_str, ct, body_len, body ? body : "") < 0)
        {
            log_error("server_response: OOM building response",
                      "status", status_buf, "req_id", client->req_id, NULL);
            return -1;
        }
    }

    size_t resp_len = strlen(resp);
    uv_buf_t buf = {.base = resp, .len = resp_len};
    uv_write_t *req = malloc(sizeof(uv_write_t));
    WriteCtx *wctx = malloc(sizeof(WriteCtx));
    if (!req || !wctx)
    {
        log_error("server_response: OOM allocating write request",
                  "status", status_buf, "req_id", client->req_id, NULL);
        free(wctx);
        free(req);
        free(resp);
        return -1;
    }
    wctx->client = client;
    wctx->buf = resp;
    req->data = wctx;
    if (uv_write(req, (uv_stream_t *)&client->handle, &buf, 1, write_done) != 0)
    {
        /* The request was never queued, so write_done will never run —
         * free the packet now and tear the connection down (no response
         * can reach a broken stream). */
        log_error("server_response: uv_write failed",
                  "status", status_buf, "req_id", client->req_id, NULL);
        free(wctx->buf);
        free(wctx);
        free(req);
        client_close(client);
        return -1;
    }
    return 0;
}

int server_response_json(Client *client, int status, const char *json)
{
    return server_response(client, status, "application/json", json);
}

int server_response_error(Client *client, int status, const char *msg)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", msg);
    char *str = cJSON_PrintUnformatted(json);
    int rc = server_response_json(client, status, str);
    free(str);
    cJSON_Delete(json);
    return rc;
}


/* Loop-thread only: increments non-atomically, so it must never be
 * read or written off the libuv loop thread (ids are only used for
 * request tracing on the same thread). */
static unsigned long req_counter = 0;

static void gen_req_id(char *buf, size_t len)
{
    req_counter++;
    snprintf(buf, len, "%lu", req_counter);
}

static void handle_request(Client *client)
{
    client->headers_done = 1;
    gen_req_id(client->req_id, sizeof(client->req_id));

    if (client->ctx->rate_limiter &&
        !rate_limiter_allow(client->ctx->rate_limiter, client->ip))
    {
        server_response_error(client, 429, "too many requests");
        return;
    }

    log_info("request", "method", client->method, "path", client->path,
             "req_id", client->req_id, "ip", client->ip, NULL);

    if (strcmp(client->method, "OPTIONS") == 0)
    {
        server_response(client, 204, NULL, NULL);
        return;
    }

    if (strcmp(client->path, "/ws/chat") == 0 && strcmp(client->method, "GET") == 0)
    {
        HTTPRequest req;
        memset(&req, 0, sizeof(req));
        memcpy(req.method, client->method, sizeof(req.method) - 1);
        memcpy(req.path, client->path, sizeof(req.path) - 1);
        memcpy(req.query, client->query, sizeof(req.query) - 1);
        memcpy(req.headers, client->headers, sizeof(req.headers) - 1);
        req.body = client->body;
        req.body_len = client->body_len;
        memcpy(&req.addr, &client->addr, sizeof(req.addr));
        memcpy(req.ip, client->ip, sizeof(req.ip) - 1);
        req.client = client;
        memcpy(req.req_id, client->req_id, sizeof(req.req_id));

        if (client->ctx->state != STATE_UNLOCKED || !client->ctx->unlock_token ||
            (strcmp(client->ctx->unlock_token, "noop") != 0 &&
             !middleware_has_valid_ws_token(req.headers, client->ctx->unlock_token)))
        {
            server_response_error(client, 401, "unauthorized");
            return;
        }

        int rc = ws_do_handshake(&req, client, client->ctx);
        if (rc != 0)
            server_response_error(client, 400, "websocket upgrade failed");
        return;
    }

    for (int i = 0; i < routes_count; i++)
    {
        if (route_match(client->method, client->path, &routes[i]))
        {
            HTTPRequest req;
            memset(&req, 0, sizeof(req));
            memcpy(req.method, client->method, sizeof(req.method) - 1);
            memcpy(req.path, client->path, sizeof(req.path) - 1);
            memcpy(req.query, client->query, sizeof(req.query) - 1);
            memcpy(req.headers, client->headers, sizeof(req.headers) - 1);
            req.body = client->body;
            req.body_len = client->body_len;
            memcpy(&req.addr, &client->addr, sizeof(req.addr));
            memcpy(req.ip, client->ip, sizeof(req.ip) - 1);
            req.client = client;
            memcpy(req.req_id, client->req_id, sizeof(req.req_id));

            if (routes[i].needs_unlock)
            {
                int authorized;
                if (routes[i].unlock_via_query)
                    authorized = middleware_check_unlock_query(&req, client->ctx);
                else
                    authorized = middleware_check_unlock(&req, client->ctx);
                if (!authorized)
                {
                    server_response_error(client, 401, "unauthorized");
                    return;
                }
            }

            routes[i].handler(&req, client, client->ctx);
            return;
        }
    }

    if (strcmp(client->method, "GET") == 0)
    {
        serve_static(client, client->path, client->ctx);
        return;
    }

    server_response_error(client, 404, "not found");
}



#ifdef SERVER_TEST
void server_test_free_reset(void) { server_test_free_count = 0; }
int server_test_free_tally(void) { return server_test_free_count; }

/* Stack-free test client: calloc'd so every field is safe to free; freed
 * by client_close_cb when the tests trigger the close path. handle.data is
 * wired like the real on_connection path so client_close_cb finds the
 * client. */
Client *server_test_client_new(void)
{
    Client *c = calloc(1, sizeof(Client));
    if (!c) return NULL;
    snprintf(c->req_id, sizeof(c->req_id), "test");
    /* handle.data is wired like the real on_connection path so
     * client_close_cb finds the client (data is uv_tcp_t's first member). */
    c->handle.data = c;
    return c;
}

/* Releases a client without going through uv_close — for tests whose code
 * path (SSE) deliberately keeps the client open. */
void server_test_client_free(Client *client)
{
    if (!client) return;
    free(client->buf);
    free(client->body);
    free(client->ws_private);
    free(client);
}

#endif

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    Client *client = (Client *)stream->data;
    if (!client) {
        free(buf->base);
        return;
    }

    if (nread < 0)
    {
        if (nread != UV_EOF) log_error("read error", NULL);
        free(buf->base);
        if (client->is_ws)
            client_close_cb((uv_handle_t *)stream);
        else
            client_close(client);
        return;
    }

    if (nread == 0) {
        free(buf->base);
        return;
    }

    if (client->is_ws)
    {
        free(buf->base);
        return;
    }

    int append_rc = client_append(client, buf->base, (size_t)nread);
    free(buf->base);

    if (append_rc != 0)
    {
        uv_read_stop(stream);
        server_response_error(client, 413, "request too large");
        return;
    }

    int rc = parse_http(client);

    if (rc < 0)
    {
        uv_read_stop(stream);
        server_response_error(client, 400, "bad request");
        return;
    }

    if (rc == 1)
    {
        uv_read_stop(stream);
        handle_request(client);
    }
}

static void on_connection(uv_stream_t *server, int status)
{
    if (status < 0)
    {
        log_error("on_connection: accept callback error", "err", uv_strerror(status), NULL);
        return;
    }

    Client *client = calloc(1, sizeof(Client));
    if (!client)
    {
        log_error("on_connection: OOM allocating client", NULL);
        return;
    }

    /* C11: init failures were dropped with no trace; on a failure the
     * connection cannot be served, so the client is released. */
    if (uv_tcp_init(server->loop, &client->handle) != 0)
    {
        log_error("on_connection: uv_tcp_init failed", NULL);
        free(client);
        return;
    }
    client->handle.data = client;

    if (uv_accept(server, (uv_stream_t *)&client->handle) != 0)
    {
        log_error("on_connection: uv_accept failed", NULL);
        free(client);
        return;
    }

    ServerContext *ctx = (ServerContext *)server->data;
    client->ctx = ctx;

    int namelen = sizeof(client->addr);
    if (uv_tcp_getpeername(&client->handle, (struct sockaddr *)&client->addr,
                           &namelen) != 0)
    {
        log_error("on_connection: getpeername failed", NULL);
    }
    if (client->addr.ss_family == AF_INET)
    {
        struct sockaddr_in *sin = (struct sockaddr_in *)&client->addr;
        uv_ip4_name(sin, client->ip, sizeof(client->ip));
    }

    uv_read_start((uv_stream_t *)&client->handle, alloc_cb, read_cb);
}

int server_start(ServerContext *ctx)
{
    ctx->loop = uv_default_loop();
    (void)signal(SIGPIPE, SIG_IGN);

    uv_tcp_t *server = malloc(sizeof(uv_tcp_t));
    if (!server) return -1;

    uv_tcp_init(ctx->loop, server);
    server->data = ctx;

    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", ctx->port, &addr);

    uv_tcp_bind(server, (const struct sockaddr *)&addr, 0);

    int rc = uv_listen((uv_stream_t *)server, 128, on_connection);
    if (rc != 0)
    {
        log_error("uv_listen failed", NULL);
        free(server);
        return -1;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", ctx->port);
    log_info("server listening", "port", port_str, NULL);
    printf("Echo AI server started on http://localhost:%d\n", ctx->port);
    fflush(stdout);

    uv_run(ctx->loop, UV_RUN_DEFAULT);
    return 0;
}

void server_stop(ServerContext *ctx)
{
    /* Doc contract (server.h): NULL is a no-op. */
    if (ctx && ctx->loop) uv_stop(ctx->loop);
}
