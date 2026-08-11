/*
 * server_internal.h - shared state for the HTTP server, split across
 * server (core), http_parse, and serve_static units. Documented
 * exception to the one-header-per-module rule: the Client connection
 * struct is mutated by all three units, so it lives here instead of
 * being duplicated. Not installed or included outside src/server.
 * Depends on: server.h, libuv.
 */

#ifndef ECHO_SERVER_INTERNAL_H
#define ECHO_SERVER_INTERNAL_H

#include "server.h"

struct Client {
    uv_tcp_t handle;
    char *buf;
    size_t buf_len;
    size_t buf_cap;
    size_t header_len;
    char method[16];
    char path[1024];
    char query[512];
    char headers[4096];
    char *body;
    size_t body_len;
    int body_read;
    int content_length;
    int headers_done;
    int is_ws;
    void *ws_private;
    int closed;
    int response_status;
    char req_id[32];
    struct sockaddr_storage addr;
    char ip[64];
    ServerContext *ctx;
};

#define MAX_HTTP_HEADER_SIZE 16384U
#define MAX_HTTP_BODY_SIZE 10485760U

#endif /* ECHO_SERVER_INTERNAL_H */
