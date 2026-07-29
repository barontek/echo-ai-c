#ifndef ECHO_ROUTES_SESSION_H
#define ECHO_ROUTES_SESSION_H

#include "routes.h"

void handle_sessions(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_create_session(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_session_get(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_session_delete(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_session_update(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_sessions_rename(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_session_import(HTTPRequest *req, Client *client, ServerContext *ctx);

#endif
