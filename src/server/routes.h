#ifndef ECHO_ROUTES_H
#define ECHO_ROUTES_H

#include "server.h"

typedef struct WSClient WSClient;

typedef struct {
    const char *method;
    const char *path;
    int is_prefix;
    int needs_unlock;
    route_handler handler;
} Route;

extern const Route routes[];
extern const int routes_count;

int route_match(const char *method, const char *path, const Route *r);

void routes_ws_chat_init(WSClient *ws, ServerContext *ctx, const char *query);
void server_sse_write(Client *client, const char *data);

#endif
