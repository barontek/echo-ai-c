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

static const char *header_value(const char *headers, const char *name,
                                size_t *value_len)
{
    size_t name_len = strlen(name);
    const char *line = headers;

    while (line && *line)
    {
        const char *newline = strchr(line, '\n');
        const char *line_end = newline ? newline : line + strlen(line);
        if (line_end > line && line_end[-1] == '\r') line_end--;

        const char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon && (size_t)(colon - line) == name_len &&
            strncasecmp(line, name, name_len) == 0)
        {
            const char *value = colon + 1;
            while (value < line_end && (*value == ' ' || *value == '\t')) value++;
            const char *end = line_end;
            while (end > value && (end[-1] == ' ' || end[-1] == '\t')) end--;
            *value_len = (size_t)(end - value);
            return value;
        }

        if (!newline) break;
        line = newline + 1;
    }

    return NULL;
}

int middleware_has_valid_token(const char *headers, const char *token)
{
    if (!headers || !token) return 0;

    size_t value_len = 0;
    const char *value = header_value(headers, "X-Unlock-Token", &value_len);
    return value && token_equals(value, value_len, token);
}

int middleware_has_valid_ws_token(const char *headers, const char *token)
{
    if (!headers || !token) return 0;

    size_t value_len = 0;
    const char *value = header_value(headers, "Sec-WebSocket-Protocol", &value_len);
    if (!value) return 0;

    static const char prefix[] = "echo-ai-token-";
    const char *end = value + value_len;
    const char *item = value;
    while (item < end)
    {
        while (item < end && (*item == ' ' || *item == '\t' || *item == ',')) item++;
        const char *item_end = item;
        while (item_end < end && *item_end != ',') item_end++;
        const char *trimmed_end = item_end;
        while (trimmed_end > item &&
               (trimmed_end[-1] == ' ' || trimmed_end[-1] == '\t')) trimmed_end--;

        size_t item_len = (size_t)(trimmed_end - item);
        size_t prefix_len = sizeof(prefix) - 1;
        if (item_len >= prefix_len &&
            memcmp(item, prefix, sizeof(prefix) - 1) == 0 &&
            token_equals(item + prefix_len, item_len - prefix_len, token))
            return 1;

        item = item_end;
    }

    return 0;
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
