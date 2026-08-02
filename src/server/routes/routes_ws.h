#ifndef ECHO_ROUTES_WS_H
#define ECHO_ROUTES_WS_H

#include "routes.h"

void routes_ws_chat_init(WSClient *ws, ServerContext *ctx, const char *query);
/* Cancels active chat runs and releases their unlocked storage references. */
void routes_ws_invalidate_auth(ServerContext *ctx);

#endif
