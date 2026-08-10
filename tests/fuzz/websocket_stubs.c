/* Minimal link stubs for fuzz targets that compile src/server/websocket.c
 * under WEBSOCKET_TEST (same symbols test_websocket.c provides). The
 * frame-walking hook under test never calls these at runtime; they exist
 * only so the linker can resolve the rest of the TU. */

#include <stdarg.h>
#include <stdint.h>
#include <uv.h>

#include "server/websocket.h"
#include "server/routes/routes_ws.h"
#include "utils/logging.h"

void routes_ws_chat_init(WSClient *ws, ServerContext *ctx, const char *query)
{
    (void)ws;
    (void)ctx;
    (void)query;
}

void client_close(Client *client) { (void)client; }

void client_set_ws_private(Client *client, void *priv)
{
    (void)client;
    (void)priv;
}

void log_msg(LogLevel level, const char *file, int line,
             const char *message, ...)
{
    (void)level;
    (void)file;
    (void)line;
    (void)message;
}

int uv_read_stop(uv_stream_t *stream) { (void)stream; return 0; }

int uv_read_start(uv_stream_t *stream, uv_alloc_cb alloc_cb,
                  uv_read_cb read_cb)
{
    (void)stream;
    (void)alloc_cb;
    (void)read_cb;
    return 0;
}

uv_loop_t *uv_handle_get_loop(const uv_handle_t *handle)
{
    (void)handle;
    return (uv_loop_t *)1;
}

int uv_timer_init(uv_loop_t *loop, uv_timer_t *timer)
{
    (void)loop;
    (void)timer;
    return 0;
}

int uv_timer_start(uv_timer_t *timer, uv_timer_cb cb,
                   uint64_t timeout, uint64_t repeat)
{
    (void)timer;
    (void)cb;
    (void)timeout;
    (void)repeat;
    return 0;
}
