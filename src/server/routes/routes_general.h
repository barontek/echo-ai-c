#ifndef ECHO_ROUTES_GENERAL_H
#define ECHO_ROUTES_GENERAL_H

#include "routes.h"

void handle_status(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_health(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_config(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_metrics(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_undo(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_redo(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_health_detailed(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_models(HTTPRequest *req, Client *client, ServerContext *ctx);
void handle_providers(HTTPRequest *req, Client *client, ServerContext *ctx);

#endif
