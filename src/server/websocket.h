/*
 * websocket.h - RFC 6455 server-side websocket layer on libuv: upgrade
 * handshake, frame send/receive, and keepalive pings.
 * Depends on: libuv, OpenSSL (SHA-1/base64), server.h, routes/routes_ws.h.
 */

#ifndef ECHO_WEBSOCKET_H
#define ECHO_WEBSOCKET_H

#include <uv.h>
#include <stdint.h>
#include <time.h>

#include "server.h"

typedef struct WSClient WSClient;

/* Fired synchronously from the read callback with a NUL-terminated copy
 * of the payload; userdata is the pointer set by the init callback. */
typedef void (*ws_msg_handler)(WSClient *ws, const char *data, size_t len, void *userdata);
/* Fired once when the connection dies or a close frame arrives; the
 * handler is cleared before firing so it runs exactly once. */
typedef void (*ws_close_handler)(WSClient *ws, void *userdata);

/* Created by ws_do_handshake(); userdata is owned by the init callback
 * (routes_ws_chat_init) and released in the on_close handler. */
struct WSClient {
    uv_tcp_t *handle;
    Client *client;
    int handshake_done;
    ws_msg_handler on_message;
    ws_close_handler on_close;
    void *userdata;
    uv_timer_t ping_timer;
    time_t last_pong;
};

/**
 * ws_do_handshake - perform the RFC 6455 server upgrade handshake
 * @req: parsed request whose headers must carry Sec-WebSocket-Key.
 * @client: connection to upgrade.
 * @ctx: server context, forwarded to routes_ws_chat_init().
 *
 * Writes the 101 response, repurposes the connection so the stream's data
 * points at a new WSClient, wires the message/close callbacks, starts
 * reads and the ping timer. req->query is borrowed only for the duration
 * of the call (it is copied by routes_ws_chat_init). After success the
 * caller must not touch client — it is owned by the websocket layer until
 * the connection closes, and its ws_private owns the WSClient.
 *
 * Return: 0 on success, -1 on missing/invalid key, response-build or
 * write failure (the connection stays owned by the HTTP layer and should
 * be rejected/closed by the caller). Failures are logged. Call on the
 * loop thread; the WSClient and its callbacks live on that thread.
 */
int ws_do_handshake(HTTPRequest *req, Client *client, ServerContext *ctx);

/**
 * ws_send - send one FIN text frame
 * @ws: connection from a successful handshake; must be non-NULL.
 * @data: payload bytes, borrowed — copied into the frame before the call
 *   returns.
 * @len: payload length in bytes.
 *
 * Frames of any length are supported (126/127 extended headers as
 * needed). The frame buffer is owned by libuv until the write completes.
 *
 * Return: 0 on success, -1 on NULL ws/handle, allocation failure, or a
 * write error — in all failure cases nothing is queued. Never blocks.
 * Call on the loop thread.
 */
int ws_send(WSClient *ws, const char *data, size_t len);

/**
 * ws_send_json - send a pre-serialized JSON text frame
 * @ws: connection from a successful handshake.
 * @json: NUL-terminated JSON string, borrowed for the duration of the
 *   call.
 *
 * Convenience wrapper over ws_send(); strlen() derives the frame length.
 *
 * Return: 0 on success, -1 on failure (see ws_send()).
 */
int ws_send_json(WSClient *ws, const char *json);

/**
 * ws_send_close - send a websocket close frame
 * @ws: connection to send on; NULL is accepted and rejected.
 * @code: close status code, sent big-endian in the frame payload.
 *
 * Only queues the frame: the connection is torn down when the peer
 * replies with its own close or the read side hits EOF.
 *
 * Return: 0 on success, -1 on failure (same conditions as ws_send()).
 */
int ws_send_close(WSClient *ws, uint16_t code);

/**
 * ws_start_read - start delivering frames from the peer
 * @ws: connection that completed the handshake; must be non-NULL.
 *
 * Frames are parsed in the read callback and on_message/on_close fire
 * synchronously from it. Called internally by ws_do_handshake().
 *
 * Return: 0 on success, negative libuv error code on failure.
 */
int ws_start_read(WSClient *ws);

/**
 * ws_start_ping_timer - start the 15-second keepalive ping interval
 * @ws: connection with a live handle (handshake done); must be non-NULL.
 *
 * Records the current time as last_pong, initializes the embedded timer
 * on the connection's loop, and starts it at a 15 s interval. Call once
 * per connection, after ws_do_handshake().
 *
 * Return: void; never fails. Call on the loop thread.
 */
int ws_start_ping_timer(WSClient *ws);

/**
 * ws_stop_ping_timer - stop the keepalive ping timer
 * @ws: connection whose timer ws_start_ping_timer() started.
 *
 * Called internally on connection teardown; safe to call once the timer
 * is running. The timer handle itself is never closed separately.
 *
 * Return: void; never fails.
 */
void ws_stop_ping_timer(WSClient *ws);

#ifdef WEBSOCKET_TEST
/**
 * websocket_test_frame_walk - walk RFC 6455 frames over a raw buffer
 * @data: raw bytes; may be any length, need not be NUL-terminated.
 * @len: number of bytes in @data.
 *
 * Test-only hook sharing ws_frame_header with the real read loop (one
 * implementation, not a mirror). Returns the number of bytes consumed:
 * @len for a fully valid stream, otherwise the offset where parsing
 * stopped (truncated header or payload). Never fails; safe on any input.
 *
 * Return: bytes consumed; thread-safe (pure).
 */
size_t websocket_test_frame_walk(const unsigned char *data, size_t len);
#endif

#endif
