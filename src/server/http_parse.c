/*
 * http_parse.c - streaming HTTP request parsing: header/body framing
 * with size limits, request-line splitting, and content-length
 * validation.
 * Depends on: server_internal.h (Client, limits).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "server_internal.h"
#include "server.h"
#include "http_parse.h"

static int parse_content_length(const char *headers, size_t header_len, size_t *out)
{
    const char *line = headers;
    const char *end = headers + header_len;
    int found = 0;
    size_t value = 0;

    while (line < end)
    {
        const char *line_end = line;
        while (line_end + 1 < end &&
               !(line_end[0] == '\r' && line_end[1] == '\n'))
            line_end++;
        const char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon)
        {
            size_t name_len = (size_t)(colon - line);
            if (name_len == 14 && strncasecmp(line, "Content-Length", 14) == 0)
            {
                if (found) return -1;
                const char *p = colon + 1;
                while (p < line_end && (*p == ' ' || *p == '\t')) p++;
                if (p == line_end) return -1;
                size_t parsed = 0;
                while (p < line_end && *p >= '0' && *p <= '9')
                {
                    unsigned int digit = (unsigned int)(*p - '0');
                    if (parsed > (SIZE_MAX - digit) / 10) return -1;
                    parsed = parsed * 10 + digit;
                    p++;
                }
                while (p < line_end && (*p == ' ' || *p == '\t')) p++;
                if (p != line_end || parsed > MAX_HTTP_BODY_SIZE) return -1;
                value = parsed;
                found = 1;
            }
            else if (name_len == 17 &&
                     strncasecmp(line, "Transfer-Encoding", 17) == 0)
            {
                return -1;
            }
        }
        if (line_end + 1 >= end) break;
        line = line_end + 2;
    }

    *out = value;
    return 0;
}

int parse_http(Client *client)
{
    const char *data = client->buf;
    size_t len = client->buf_len;

    if (!client->headers_done)
    {
        const char *header_end = strstr(data, "\r\n\r\n");
        if (!header_end)
            return len > MAX_HTTP_HEADER_SIZE ? -1 : 0;

        size_t header_len = (size_t)(header_end - data) + 4;
        if (header_len > MAX_HTTP_HEADER_SIZE ||
            header_len >= sizeof(client->headers)) return -1;
        const char *request_line_end = strstr(data, "\r\n");
        if (!request_line_end || request_line_end > header_end) return -1;
        const char *path_start = NULL;
        const char *path_end = NULL;

        path_start = memchr(data, ' ', (size_t)(request_line_end - data));
        if (!path_start) return -1;
        path_start++;

        path_end = memchr(path_start, ' ', (size_t)(request_line_end - path_start));
        if (!path_end) return -1;

        size_t method_len = (size_t)(path_start - data - 1);
        if (method_len == 0 || method_len >= sizeof(client->method)) return -1;
        memcpy(client->method, data, method_len);
        client->method[method_len] = '\0';

        size_t path_len = path_end - path_start;
        const char *qs = memchr(path_start, '?', path_len);
        if (qs)
        {
            size_t p_len = qs - path_start;
            if (p_len >= sizeof(client->path)) return -1;
            memcpy(client->path, path_start, p_len);
            client->path[p_len] = '\0';
            size_t q_len = path_len - (qs - path_start) - 1;
            if (q_len >= sizeof(client->query)) return -1;
            memcpy(client->query, qs + 1, q_len);
            client->query[q_len] = '\0';
        }
        else
        {
            if (path_len >= sizeof(client->path)) return -1;
            memcpy(client->path, path_start, path_len);
            client->path[path_len] = '\0';
        }

        memcpy(client->headers, data, header_len);
        client->headers[header_len] = '\0';

        size_t content_length = 0;
        if (parse_content_length(data, header_len, &content_length) != 0) return -1;
        client->content_length = (int)content_length;
        client->header_len = header_len;

        client->headers_done = 1;
    }

    size_t available = len - client->header_len;
    if (available < (size_t)client->content_length) return 0;

    if (client->content_length > 0 && !client->body)
    {
        client->body = malloc((size_t)client->content_length + 1);
        if (!client->body) return -1;
        memcpy(client->body, data + client->header_len,
               (size_t)client->content_length);
        client->body_len = (size_t)client->content_length;
        client->body[client->body_len] = '\0';
    }
    return 1;
}

int client_append(Client *client, const char *data, size_t len)
{
    if (len > SIZE_MAX - client->buf_len - 1) return -1;
    size_t needed = client->buf_len + len + 1;
    if (needed > MAX_HTTP_HEADER_SIZE + MAX_HTTP_BODY_SIZE + 1) return -1;
    if (needed > client->buf_cap)
    {
        size_t new_cap = client->buf_cap ? client->buf_cap : 4096;
        while (new_cap < needed)
        {
            if (new_cap > SIZE_MAX / 2) return -1;
            new_cap *= 2;
        }
        char *new_buf = realloc(client->buf, new_cap);
        if (!new_buf) return -1;
        client->buf = new_buf;
        client->buf_cap = new_cap;
    }
    memcpy(client->buf + client->buf_len, data, len);
    client->buf_len += len;
    client->buf[client->buf_len] = '\0';
    return 0;
}

#ifdef SERVER_TEST
int server_test_parse_chunks(const char **chunks, const size_t *lengths, int count,
                             char *method, size_t method_size,
                             char *path, size_t path_size,
                             char *body, size_t body_size)
{
    Client client = {0};
    int rc = 0;
    for (int i = 0; i < count; i++)
    {
        if (client_append(&client, chunks[i], lengths[i]) != 0)
        {
            rc = -1;
            break;
        }
        rc = parse_http(&client);
        if (rc < 0) break;
    }
    if (rc == 1)
    {
        if (snprintf(method, method_size, "%s", client.method) < 0 ||
            snprintf(path, path_size, "%s", client.path) < 0 ||
            snprintf(body, body_size, "%s", client.body ? client.body : "") < 0)
            rc = -1;
    }
    free(client.buf);
    free(client.body);
    return rc;
}
#endif
