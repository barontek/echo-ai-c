#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "middleware.h"
#include "../utils/logging.h"

int middleware_has_valid_token(const char *headers, const char *token)
{
    if (!headers || !token) return 0;

    size_t hlen = strlen(headers);
    char *headers_lower = malloc(hlen + 1);
    if (!headers_lower) return 0;
    for (size_t i = 0; i < hlen; i++)
        headers_lower[i] = (headers[i] >= 'A' && headers[i] <= 'Z')
                           ? headers[i] + 32 : headers[i];
    headers_lower[hlen] = '\0';

    const char *found = strstr(headers_lower, "x-unlock-token:");
    int ret = 0;
    if (found)
    {
        int idx = (int)(found - headers_lower);
        const char *tok = headers + idx + 15;
        while (*tok == ' ') tok++;
        if (strncmp(tok, token, strlen(token)) == 0)
            ret = 1;
    }
    free(headers_lower);
    return ret;
}

int middleware_check_unlock(HTTPRequest *req, ServerContext *ctx)
{
    if (ctx->state == STATE_UNLOCKED && ctx->unlock_token)
        return middleware_has_valid_token(req->headers, ctx->unlock_token);
    return 0;
}

int middleware_check_rate_limit(HTTPRequest *req, ServerContext *ctx)
{
    if (!ctx->rate_limiter) return 1;
    return rate_limiter_allow(ctx->rate_limiter, req->ip);
}

void middleware_add_cors(Client *client, ServerContext *ctx)
{
    (void)ctx;
    server_response(client, 204, NULL, NULL);
}
