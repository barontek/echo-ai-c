#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/crypto.h>

#include "middleware.h"
#include "../utils/logging.h"

int token_equals(const char *a, size_t a_len, const char *b)
{
    if (!a || !b) return 0;
    size_t b_len = strlen(b);
    if (a_len != b_len) return 0;
    return CRYPTO_memcmp(a, b, a_len) == 0;
}

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

        size_t sent_len = 0;
        while (tok[sent_len] != '\r' && tok[sent_len] != '\n'
               && tok[sent_len] != '\0')
            sent_len++;

        if (token_equals(tok, sent_len, token))
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

int middleware_check_unlock_query(HTTPRequest *req, ServerContext *ctx)
{
    if (!req || !ctx) return 0;
    if (ctx->state != STATE_UNLOCKED || !ctx->unlock_token)
        return 0;

    /* NOTE: passing the unlock token in a URL query parameter means it
     * can end up in browser history, referrer headers, and server logs.
     * EventSource (SSE) cannot set custom headers, so query param is
     * the only viable transport for browser-native EventSource usage. */

    const char *q = req->query;
    if (!q || !q[0]) return 0;

    const char *p = strstr(q, "token=");
    if (!p) return 0;

    p += 6;
    size_t tlen = 0;
    while (p[tlen] != '&' && p[tlen] != '\0')
        tlen++;

    if (tlen == 0) return 0;

    return token_equals(p, tlen, ctx->unlock_token);
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
