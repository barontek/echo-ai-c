#ifndef ECHO_WEBSOCKET_H
#define ECHO_WEBSOCKET_H

#include <uv.h>
#include <stdint.h>
#include <time.h>

#include "server.h"

typedef struct WSClient WSClient;

typedef void (*ws_msg_handler)(WSClient *ws, const char *data, size_t len, void *userdata);
typedef void (*ws_close_handler)(WSClient *ws, void *userdata);

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

int ws_do_handshake(HTTPRequest *req, Client *client, ServerContext *ctx);
int ws_send(WSClient *ws, const char *data, size_t len);
int ws_send_json(WSClient *ws, const char *json);
int ws_send_close(WSClient *ws, uint16_t code);
int ws_start_read(WSClient *ws);
void ws_start_ping_timer(WSClient *ws);
void ws_stop_ping_timer(WSClient *ws);

#endif
