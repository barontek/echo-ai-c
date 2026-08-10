/*
 * server.c - libuv HTTP server core: connection accept, streaming request
 * parser with size limits, static file serving, and the response helpers
 * the route handlers use.
 * Depends on: libuv, cJSON, routes/, websocket.h, middleware.h,
 * utils/ (logging, string_utils, rate_limiter).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <cjson/cJSON.h>

#include "server.h"
#include "routes/routes.h"
#include "websocket.h"
#include "middleware.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"
#include "../utils/rate_limiter.h"

struct Client {
    uv_tcp_t handle;
    char *buf;
    size_t buf_len;
    size_t buf_cap;
    size_t header_len;
    char method[16];
    char path[1024];
    char query[512];
    char headers[4096];
    char *body;
    size_t body_len;
    int body_read;
    int content_length;
    int headers_done;
    int is_ws;
    void *ws_private;
    int closed;
    int response_status;
    char req_id[32];
    struct sockaddr_storage addr;
    char ip[64];
    ServerContext *ctx;
};

#define MAX_HTTP_HEADER_SIZE 16384U
#define MAX_HTTP_BODY_SIZE 10485760U

static const char *mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(ext, ".css") == 0)  return "text/css; charset=utf-8";
    if (strcmp(ext, ".js") == 0)   return "application/javascript; charset=utf-8";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0)  return "image/png";
    if (strcmp(ext, ".svg") == 0)  return "image/svg+xml";
    if (strcmp(ext, ".ico") == 0)  return "image/x-icon";
    return "application/octet-stream";
}

static int static_request_path_safe(const char *path)
{
    if (!path || path[0] != '/' || strchr(path, '\\')) return 0;

    const char *segment = path;
    while (*segment)
    {
        while (*segment == '/') segment++;
        const char *end = strchr(segment, '/');
        size_t len = end ? (size_t)(end - segment) : strlen(segment);
        if ((len == 1 && segment[0] == '.') ||
            (len == 2 && segment[0] == '.' && segment[1] == '.'))
            return 0;
        if (!end) break;
        segment = end;
    }
    return 1;
}

static int open_static_file_beneath(const char *root, const char *filepath)
{
    size_t root_len = strlen(root);
    if (strncmp(filepath, root, root_len) != 0 ||
        (filepath[root_len] != '\0' && filepath[root_len] != '/'))
        return -1;

    const char *relative = filepath + root_len;
    while (*relative == '/') relative++;
    if (!*relative || strlen(relative) >= PATH_MAX) return -1;

    char path_copy[PATH_MAX];
    memcpy(path_copy, relative, strlen(relative) + 1);
    int dir_fd = open(root, O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) return -1;

    char *save = NULL;
    char *segment = strtok_r(path_copy, "/", &save);
    while (segment)
    {
        char *next = strtok_r(NULL, "/", &save);
        int flags = O_RDONLY | O_NOFOLLOW;
        if (next) flags |= O_DIRECTORY;
        int next_fd = openat(dir_fd, segment, flags);
        close(dir_fd);
        if (next_fd < 0) return -1;
        dir_fd = next_fd;
        segment = next;
    }
    return dir_fd;
}

void client_set_ws_private(Client *client, void *priv)
{
    client->ws_private = priv;
}

static void client_close_cb(uv_handle_t *handle)
{
    Client *client = (Client *)handle->data;
    if (client)
    {
        free(client->buf);
        free(client->body);
        free(client->ws_private);
        memset(client, 0, sizeof(Client));
        free(client);
    }
}

void client_close(Client *client)
{
    if (!client || client->closed) return;
    client->closed = 1;
    if (!uv_is_closing((uv_handle_t *)&client->handle))
        uv_close((uv_handle_t *)&client->handle, client_close_cb);
    else
        client_close_cb((uv_handle_t *)&client->handle);
}

static void alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf)
{
    (void)handle;
    buf->base = malloc(size);
    buf->len = size;
}

static void write_done(uv_write_t *req, int status)
{
    (void)status;
    Client *client = (Client *)req->data;
    free(req);
    if (client && !client->is_ws)
        client_close(client);
}

static void sse_write_done(uv_write_t *req, int status)
{
    (void)status;
    free(req->data);
    free(req);
}

/* SSE frame writer for streaming routes: data is duplicated into a
 * self-freeing uv_write request, so the caller can hand back stack
 * buffers. Declared in routes/routes.h; HTTP only — websocket clients
 * are ignored. Failures (allocation, queue full) drop the frame silently.
 * Non-atomic per frame; the loop serializes writes per handle. */
int server_sse_write(Client *client, const char *data)
{
    if (!client || client->is_ws) return -1;
    char *copy = str_dup(data);
    if (!copy)
    {
        log_error("server_sse_write: OOM duplicating frame", NULL);
        return -1;
    }
    uv_buf_t buf = {.base = copy, .len = strlen(copy)};
    uv_write_t *req = malloc(sizeof(uv_write_t));
    if (!req)
    {
        log_error("server_sse_write: OOM allocating write request", NULL);
        free(copy);
        return -1;
    }
    req->data = copy;
    uv_write(req, (uv_stream_t *)&client->handle, &buf, 1, sse_write_done);
    return 0;
}

int server_response(Client *client, int status, const char *content_type, const char *body)
{
    if (!client || client->is_ws) return -1;

    client->response_status = status;
    char status_buf[16];
    snprintf(status_buf, sizeof(status_buf), "%d", status);
    log_info("response", "status", status_buf,
             "req_id", client->req_id, "path", client->path, NULL);

    char *resp = NULL;
    size_t body_len = body ? strlen(body) : 0;

    const char *status_str = "OK";
    if (status == 404) status_str = "Not Found";
    else if (status == 401) status_str = "Unauthorized";
    else if (status == 400) status_str = "Bad Request";
    else if (status == 413) status_str = "Payload Too Large";
    else if (status == 500) status_str = "Internal Server Error";
    else if (status == 204) status_str = "No Content";

    const char *ct = content_type ? content_type : "text/plain; charset=utf-8";

    if (status == 204)
    {
        if (asprintf(&resp,
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, X-Unlock-Token\r\n"
            "\r\n") < 0)
        {
            log_error("server_response: OOM building 204 response",
                      "req_id", client->req_id, NULL);
            return -1;
        }
    }
    else
    {
        if (asprintf(&resp,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, X-Unlock-Token\r\n"
            "\r\n"
            "%s",
            status, status_str, ct, body_len, body ? body : "") < 0)
        {
            log_error("server_response: OOM building response",
                      "status", status_buf, "req_id", client->req_id, NULL);
            return -1;
        }
    }

    size_t resp_len = strlen(resp);
    uv_buf_t buf = {.base = resp, .len = resp_len};
    uv_write_t *req = malloc(sizeof(uv_write_t));
    if (!req)
    {
        log_error("server_response: OOM allocating write request",
                  "status", status_buf, "req_id", client->req_id, NULL);
        free(resp);
        return -1;
    }
    req->data = client;
    uv_write(req, (uv_stream_t *)&client->handle, &buf, 1, write_done);
    return 0;
}

int server_response_json(Client *client, int status, const char *json)
{
    return server_response(client, status, "application/json", json);
}

int server_response_error(Client *client, int status, const char *msg)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", msg);
    char *str = cJSON_PrintUnformatted(json);
    int rc = server_response_json(client, status, str);
    free(str);
    cJSON_Delete(json);
    return rc;
}

static void serve_static(Client *client, const char *path, ServerContext *ctx)
{
    (void)ctx;
    if (!static_request_path_safe(path))
    {
        server_response_error(client, 400, "invalid static path");
        return;
    }

    char root[PATH_MAX];
    if (!realpath("frontend/dist", root))
    {
        server_response_error(client, 404, "not found");
        return;
    }

    char candidate[PATH_MAX];
    int written = 0;
    if (strcmp(path, "/") == 0)
        written = snprintf(candidate, sizeof(candidate), "%s/index.html", root);
    else
        written = snprintf(candidate, sizeof(candidate), "%s%s", root, path);
    if (written < 0 || (size_t)written >= sizeof(candidate))
    {
        server_response_error(client, 400, "static path too long");
        return;
    }

    char filepath[PATH_MAX];
    if (!realpath(candidate, filepath))
    {
        written = snprintf(candidate, sizeof(candidate), "%s%s/index.html", root, path);
        if (written < 0 || (size_t)written >= sizeof(candidate) ||
            !realpath(candidate, filepath))
        {
            server_response_error(client, 404, "not found");
            return;
        }
    }

    size_t root_len = strlen(root);
    if (strncmp(filepath, root, root_len) != 0 ||
        (filepath[root_len] != '\0' && filepath[root_len] != '/'))
    {
        server_response_error(client, 404, "not found");
        return;
    }

    int fd = open_static_file_beneath(root, filepath);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    {
        if (fd >= 0) close(fd);
        server_response_error(client, 404, "not found");
        return;
    }

    FILE *f = fdopen(fd, "rb");
    if (!f)
    {
        close(fd);
        server_response_error(client, 404, "not found");
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    if (fsize <= 0 || fsize > 10485760) { fclose(f); server_response_error(client, 500, "file too large"); return; }

    char *content = malloc((size_t)fsize + 1);
    if (!content) { fclose(f); server_response_error(client, 500, "oom"); return; }

    size_t read = fread(content, 1, (size_t)fsize, f);
    fclose(f);
    content[read] = '\0';

    const char *ct = mime_type(filepath);
    server_response(client, 200, ct, content);
    free(content);
}

static unsigned long req_counter = 0;

static void gen_req_id(char *buf, size_t len)
{
    req_counter++;
    snprintf(buf, len, "%lu", req_counter);
}

static void handle_request(Client *client)
{
    client->headers_done = 1;
    gen_req_id(client->req_id, sizeof(client->req_id));

    if (client->ctx->rate_limiter &&
        !rate_limiter_allow(client->ctx->rate_limiter, client->ip))
    {
        server_response_error(client, 429, "too many requests");
        return;
    }

    log_info("request", "method", client->method, "path", client->path,
             "req_id", client->req_id, "ip", client->ip, NULL);

    if (strcmp(client->method, "OPTIONS") == 0)
    {
        server_response(client, 204, NULL, NULL);
        return;
    }

    if (strcmp(client->path, "/ws/chat") == 0 && strcmp(client->method, "GET") == 0)
    {
        HTTPRequest req;
        memset(&req, 0, sizeof(req));
        memcpy(req.method, client->method, sizeof(req.method) - 1);
        memcpy(req.path, client->path, sizeof(req.path) - 1);
        memcpy(req.query, client->query, sizeof(req.query) - 1);
        memcpy(req.headers, client->headers, sizeof(req.headers) - 1);
        req.body = client->body;
        req.body_len = client->body_len;
        memcpy(&req.addr, &client->addr, sizeof(req.addr));
        memcpy(req.ip, client->ip, sizeof(req.ip) - 1);
        req.client = client;
        memcpy(req.req_id, client->req_id, sizeof(req.req_id));

        if (client->ctx->state != STATE_UNLOCKED || !client->ctx->unlock_token ||
            (strcmp(client->ctx->unlock_token, "noop") != 0 &&
             !middleware_has_valid_ws_token(req.headers, client->ctx->unlock_token)))
        {
            server_response_error(client, 401, "unauthorized");
            return;
        }

        int rc = ws_do_handshake(&req, client, client->ctx);
        if (rc != 0)
            server_response_error(client, 400, "websocket upgrade failed");
        return;
    }

    for (int i = 0; i < routes_count; i++)
    {
        if (route_match(client->method, client->path, &routes[i]))
        {
            HTTPRequest req;
            memset(&req, 0, sizeof(req));
            memcpy(req.method, client->method, sizeof(req.method) - 1);
            memcpy(req.path, client->path, sizeof(req.path) - 1);
            memcpy(req.query, client->query, sizeof(req.query) - 1);
            memcpy(req.headers, client->headers, sizeof(req.headers) - 1);
            req.body = client->body;
            req.body_len = client->body_len;
            memcpy(&req.addr, &client->addr, sizeof(req.addr));
            memcpy(req.ip, client->ip, sizeof(req.ip) - 1);
            req.client = client;
            memcpy(req.req_id, client->req_id, sizeof(req.req_id));

            if (routes[i].needs_unlock)
            {
                int authorized;
                if (routes[i].unlock_via_query)
                    authorized = middleware_check_unlock_query(&req, client->ctx);
                else
                    authorized = middleware_check_unlock(&req, client->ctx);
                if (!authorized)
                {
                    server_response_error(client, 401, "unauthorized");
                    return;
                }
            }

            routes[i].handler(&req, client, client->ctx);
            return;
        }
    }

    if (strcmp(client->method, "GET") == 0)
    {
        serve_static(client, client->path, client->ctx);
        return;
    }

    server_response_error(client, 404, "not found");
}

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

static int parse_http(Client *client)
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

static int client_append(Client *client, const char *data, size_t len)
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

int server_test_static_path_safe(const char *path)
{
    return static_request_path_safe(path);
}

int server_test_open_static_file_beneath(const char *root, const char *path)
{
    return open_static_file_beneath(root, path);
}
#endif

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    Client *client = (Client *)stream->data;
    if (!client) { free(buf->base); return; }

    if (nread < 0)
    {
        if (nread != UV_EOF) log_error("read error", NULL);
        free(buf->base);
        if (client->is_ws)
            client_close_cb((uv_handle_t *)stream);
        else
            client_close(client);
        return;
    }

    if (nread == 0) { free(buf->base); return; }

    if (client->is_ws)
    {
        free(buf->base);
        return;
    }

    int append_rc = client_append(client, buf->base, (size_t)nread);
    free(buf->base);

    if (append_rc != 0)
    {
        uv_read_stop(stream);
        server_response_error(client, 413, "request too large");
        return;
    }

    int rc = parse_http(client);

    if (rc < 0)
    {
        uv_read_stop(stream);
        server_response_error(client, 400, "bad request");
        return;
    }

    if (rc == 1)
    {
        uv_read_stop(stream);
        handle_request(client);
    }
}

static void on_connection(uv_stream_t *server, int status)
{
    if (status < 0) return;

    Client *client = calloc(1, sizeof(Client));
    if (!client) return;

    uv_tcp_init(server->loop, &client->handle);
    client->handle.data = client;

    if (uv_accept(server, (uv_stream_t *)&client->handle) != 0)
    {
        free(client);
        return;
    }

    ServerContext *ctx = (ServerContext *)server->data;
    client->ctx = ctx;

    int namelen = sizeof(client->addr);
    uv_tcp_getpeername(&client->handle, (struct sockaddr *)&client->addr, &namelen);
    if (client->addr.ss_family == AF_INET)
    {
        struct sockaddr_in *sin = (struct sockaddr_in *)&client->addr;
        uv_ip4_name(sin, client->ip, sizeof(client->ip));
    }

    uv_read_start((uv_stream_t *)&client->handle, alloc_cb, read_cb);
}

int server_start(ServerContext *ctx)
{
    ctx->loop = uv_default_loop();
    (void)signal(SIGPIPE, SIG_IGN);

    uv_tcp_t *server = malloc(sizeof(uv_tcp_t));
    if (!server) return -1;

    uv_tcp_init(ctx->loop, server);
    server->data = ctx;

    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", ctx->port, &addr);

    uv_tcp_bind(server, (const struct sockaddr *)&addr, 0);

    int rc = uv_listen((uv_stream_t *)server, 128, on_connection);
    if (rc != 0)
    {
        log_error("uv_listen failed", NULL);
        free(server);
        return -1;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", ctx->port);
    log_info("server listening", "port", port_str, NULL);
    printf("Echo AI server started on http://localhost:%d\n", ctx->port);
    fflush(stdout);

    uv_run(ctx->loop, UV_RUN_DEFAULT);
    return 0;
}

void server_stop(ServerContext *ctx)
{
    if (ctx->loop) uv_stop(ctx->loop);
}
