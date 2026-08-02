#ifndef ECHO_ROUTES_OPENAI_AUTH_H
#define ECHO_ROUTES_OPENAI_AUTH_H

#include "routes.h"

void handle_openai_oauth_status(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_openai_oauth_start(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_openai_oauth_logout(HTTPRequest *req, Client *client, ServerContext *ctx);

#endif
