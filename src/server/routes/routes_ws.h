#ifndef ECHO_ROUTES_WS_H
#define ECHO_ROUTES_WS_H

#include "routes.h"

void routes_ws_chat_init(WSClient *ws, ServerContext *ctx, const char *query);

#endif
