#ifndef ECHO_ROUTES_CHAT_H
#define ECHO_ROUTES_CHAT_H

#include "routes.h"

void handle_chat(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_sse_stream(HTTPRequest *req, Client *client, ServerContext *ctx);

#endif
