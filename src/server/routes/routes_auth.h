#ifndef ECHO_ROUTES_AUTH_H
#define ECHO_ROUTES_AUTH_H

#include "routes.h"

void handle_setup(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_unlock(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_logout(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_change_password(HTTPRequest *req, Client *client, ServerContext *ctx);

#endif
