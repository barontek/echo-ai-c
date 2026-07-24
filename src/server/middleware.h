#ifndef ECHO_MIDDLEWARE_H
#define ECHO_MIDDLEWARE_H

#include "server.h"

int middleware_has_valid_token(const char *headers, const char *token);
int middleware_check_unlock(HTTPRequest *req, ServerContext *ctx);
int middleware_check_rate_limit(HTTPRequest *req, ServerContext *ctx);
void middleware_add_cors(Client *client, ServerContext *ctx);

#endif
