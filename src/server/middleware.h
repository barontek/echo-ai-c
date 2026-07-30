#ifndef ECHO_MIDDLEWARE_H
#define ECHO_MIDDLEWARE_H

#include <stddef.h>
#include "server.h"

int token_equals(const char *a, size_t a_len, const char *b);
int middleware_has_valid_token(const char *headers, const char *token);
int middleware_check_unlock(HTTPRequest *req, ServerContext *ctx);
int middleware_check_unlock_query(HTTPRequest *req, ServerContext *ctx);
int middleware_check_rate_limit(HTTPRequest *req, ServerContext *ctx);
void middleware_add_cors(Client *client, ServerContext *ctx);

#endif
