#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <openssl/sha.h>
#include <openssl/evp.h>

#include "websocket.h"
#include "server.h"
#include "routes.h"
#include "../utils/logging.h"

#define WS_MAGIC "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static void ws_write_done(uv_write_t *req, int status)
{
    (void)status;
    free(req->data);
    free(req);
}

static void ws_ping_write_done(uv_write_t *req, int status)
{
    (void)status;
    free(req);
}

static void ws_ping_timer_cb(uv_timer_t *timer)
{
    WSClient *ws = (WSClient *)timer->data;
    if (!ws || !ws->handle) return;

    unsigned char ping[2] = {0x89, 0x00};
    uv_buf_t ping_buf = {.base = (char *)ping, .len = 2};
    uv_write_t *req = malloc(sizeof(uv_write_t));
    if (req) uv_write(req, (uv_stream_t *)ws->handle, &ping_buf, 1, ws_ping_write_done);
}

void ws_start_ping_timer(WSClient *ws)
{
    ws->last_pong = time(NULL);
    uv_timer_init(uv_handle_get_loop((uv_handle_t *)ws->handle), &ws->ping_timer);
    ws->ping_timer.data = ws;
    uv_timer_start(&ws->ping_timer, ws_ping_timer_cb, 15000, 15000);
}

void ws_stop_ping_timer(WSClient *ws)
{
    uv_timer_stop(&ws->ping_timer);
}

static void ws_alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf)
{
    (void)handle;
    buf->base = malloc(size);
    buf->len = size;
}

static void ws_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    WSClient *ws = (WSClient *)stream->data;
    if (!ws) { free(buf->base); return; }

    if (nread < 0)
    {
        ws_stop_ping_timer(ws);
        if (ws->on_close)
        {
            void (*on_close)(WSClient *, void *) = ws->on_close;
            ws->on_close = NULL;
            on_close(ws, ws->userdata);
        }
        free(buf->base);
        stream->data = ws->client;
        client_close(ws->client);
        return;
    }

    if (nread == 0) { free(buf->base); return; }

    unsigned char *data = (unsigned char *)buf->base;
    size_t pos = 0;

    while (pos < (size_t)nread)
    {
        if ((size_t)(nread - pos) < 2) break;

        unsigned char first = data[pos];
        unsigned char second = data[pos + 1];
        int opcode = first & 0x0F;
        int masked = (second & 0x80) ? 1 : 0;
        uint64_t payload_len = second & 0x7F;
        pos += 2;

        if (payload_len == 126)
        {
            if ((size_t)(nread - pos) < 2) break;
            payload_len = ((uint64_t)data[pos] << 8) | data[pos + 1];
            pos += 2;
        }
        else if (payload_len == 127)
        {
            if ((size_t)(nread - pos) < 8) break;
            payload_len = 0;
            for (int i = 0; i < 8; i++)
                payload_len = (payload_len << 8) | data[pos + i];
            pos += 8;
        }

        unsigned char mask_key[4] = {0, 0, 0, 0};
        if (masked)
        {
            if ((size_t)(nread - pos) < 4) break;
            memcpy(mask_key, data + pos, 4);
            pos += 4;
        }

        if ((size_t)(nread - pos) < payload_len) break;

        if (opcode == 0x8)
        {
            ws_stop_ping_timer(ws);
            if (ws->on_close)
            {
                void (*on_close)(WSClient *, void *) = ws->on_close;
                ws->on_close = NULL;
                on_close(ws, ws->userdata);
            }
            free(buf->base);
            return;
        }

        if (opcode == 0x9)
        {
            unsigned char pong[2] = {0x8A, 0x00};
            uv_buf_t pong_buf = {.base = (char *)pong, .len = 2};
            uv_write_t *req = malloc(sizeof(uv_write_t));
            if (req) uv_write(req, stream, &pong_buf, 1, ws_ping_write_done);
            free(buf->base);
            return;
        }

        if (opcode == 0xA)
        {
            ws->last_pong = time(NULL);
            free(buf->base);
            return;
        }

        if (opcode == 0x1 || opcode == 0x2)
        {
            unsigned char *payload = data + pos;
            if (masked)
            {
                for (uint64_t i = 0; i < payload_len; i++)
                    payload[i] ^= mask_key[i % 4];
            }

            if (ws->on_message)
            {
                char *text = malloc(payload_len + 1);
                if (text)
                {
                    memcpy(text, payload, payload_len);
                    text[payload_len] = '\0';
                    ws->on_message(ws, text, payload_len, ws->userdata);
                    free(text);
                }
            }
            pos += payload_len;
        }
        else
        {
            pos += payload_len;
        }
    }

    free(buf->base);
}

int ws_do_handshake(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    if (!req || !client) return -1;

    const char *h = req->headers;
    const char *key_start = strstr(h, "Sec-WebSocket-Key:");
    if (!key_start) key_start = strstr(h, "sec-websocket-key:");
    if (!key_start) { log_info("no WS key found", NULL); return -1; }

    key_start += 18;
    while (*key_start == ' ') key_start++;
    const char *key_end = key_start;
    while (*key_end && *key_end != '\r' && *key_end != '\n') key_end++;

    char combined[256];
    int clen = snprintf(combined, sizeof(combined), "%.*s%s",
                        (int)(key_end - key_start), key_start, WS_MAGIC);
    if (clen < 0 || clen >= (int)sizeof(combined)) return -1;

    unsigned char sha[SHA_DIGEST_LENGTH];
    SHA1((unsigned char *)combined, clen, sha);

    char b64[64];
    int b64_len = EVP_EncodeBlock((unsigned char *)b64, sha, SHA_DIGEST_LENGTH);
    if (b64_len <= 0) return -1;
    b64[b64_len] = '\0';
    while (b64_len > 0 && (b64[b64_len - 1] == '\n' || b64[b64_len - 1] == '\r'))
        b64[--b64_len] = '\0';

    char *resp = NULL;
    if (asprintf(&resp,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", b64) < 0)
    { return -1; }

    size_t resp_len = strlen(resp);
    uv_buf_t uvresp = {.base = resp, .len = resp_len};
    uv_write_t *wreq = malloc(sizeof(uv_write_t));
    if (!wreq) { free(resp); return -1; }
    wreq->data = resp;
    uv_write(wreq, (uv_stream_t *)req->client, &uvresp, 1, ws_write_done);

    WSClient *ws = calloc(1, sizeof(WSClient));
    if (!ws) return 0;
    ws->handle = (uv_tcp_t *)req->client;
    ws->client = (Client *)req->client;
    client_set_ws_private(ws->client, ws);
    ws->handshake_done = 1;

    log_info("websocket upgrade complete", NULL);

    uv_read_stop((uv_stream_t *)req->client);
    ((uv_stream_t *)req->client)->data = ws;
    routes_ws_chat_init(ws, ctx, req->query);
    ws_start_read(ws);
    ws_start_ping_timer(ws);

    return 0;
}

int ws_send(WSClient *ws, const char *data, size_t len)
{
    if (!ws || !ws->handle) return -1;

    size_t header_size = 2;
    if (len > 125) header_size += 2;
    if (len > 65535) header_size += 8;

    unsigned char *frame = malloc(header_size + len);
    if (!frame) return -1;

    size_t pos = 0;
    frame[pos++] = 0x81;

    if (len <= 125)
    {
        frame[pos++] = (unsigned char)len;
    }
    else if (len <= 65535)
    {
        frame[pos++] = 126;
        frame[pos++] = (unsigned char)(len >> 8);
        frame[pos++] = (unsigned char)(len & 0xFF);
    }
    else
    {
        frame[pos++] = 127;
        for (int i = 7; i >= 0; i--)
            frame[pos++] = (unsigned char)(len >> (i * 8));
    }

    memcpy(frame + pos, data, len);
    pos += len;

    uv_buf_t buf = {.base = (char *)frame, .len = pos};
    uv_write_t *req = malloc(sizeof(uv_write_t));
    if (!req) { free(frame); return -1; }

    req->data = frame;
    uv_write(req, (uv_stream_t *)ws->handle, &buf, 1, ws_write_done);
    return 0;
}

int ws_send_json(WSClient *ws, const char *json)
{
    return ws_send(ws, json, strlen(json));
}

int ws_send_close(WSClient *ws, uint16_t code)
{
    (void)code;
    unsigned char frame[4] = {0x88, 0x02, 0x03, 0xE8};
    uv_buf_t buf = {.base = (char *)frame, .len = 4};
    uv_write_t *req = malloc(sizeof(uv_write_t));
    if (!req) return -1;
    uv_write(req, (uv_stream_t *)ws->handle, &buf, 1, NULL);
    return 0;
}

int ws_start_read(WSClient *ws)
{
    return uv_read_start((uv_stream_t *)ws->handle, ws_alloc_cb, ws_read_cb);
}
