#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
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

void server_sse_write(Client *client, const char *data)
{
    if (!client || client->is_ws) return;
    char *copy = str_dup(data);
    if (!copy) return;
    uv_buf_t buf = {.base = copy, .len = strlen(copy)};
    uv_write_t *req = malloc(sizeof(uv_write_t));
    if (!req) { free(copy); return; }
    req->data = copy;
    uv_write(req, (uv_stream_t *)&client->handle, &buf, 1, sse_write_done);
}

void server_response(Client *client, int status, const char *content_type, const char *body)
{
    if (!client || client->is_ws) return;

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
            "\r\n") < 0) return;
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
            status, status_str, ct, body_len, body ? body : "") < 0) return;
    }

    size_t resp_len = strlen(resp);
    uv_buf_t buf = {.base = resp, .len = resp_len};
    uv_write_t *req = malloc(sizeof(uv_write_t));
    if (!req) { free(resp); return; }
    req->data = client;
    uv_write(req, (uv_stream_t *)&client->handle, &buf, 1, write_done);
}

void server_response_json(Client *client, int status, const char *json)
{
    server_response(client, status, "application/json", json);
}

void server_response_error(Client *client, int status, const char *msg)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", msg);
    char *str = cJSON_PrintUnformatted(json);
    server_response_json(client, status, str);
    free(str);
    cJSON_Delete(json);
}

static void serve_static(Client *client, const char *path, ServerContext *ctx)
{
    (void)ctx;
    char filepath[2048];
    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0)
        snprintf(filepath, sizeof(filepath), "frontend/dist/index.html");
    else
        snprintf(filepath, sizeof(filepath), "frontend/dist%s", path);

    struct stat st;
    if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode))
    {
        char *nf_path = NULL;
        if (asprintf(&nf_path, "frontend/dist%s/index.html", path) < 0)
        { server_response_error(client, 404, "not found"); return; }
        if (stat(nf_path, &st) == 0 && S_ISREG(st.st_mode))
        {
            free(nf_path);
            snprintf(filepath, sizeof(filepath), "frontend/dist%s/index.html", path);
        }
        else
        {
            free(nf_path);
            server_response_error(client, 404, "not found");
            return;
        }
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) { server_response_error(client, 404, "not found"); return; }

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

        /* WebSocket upgrade requests can carry the X-Unlock-Token header
         * (browsers send custom headers during the HTTP upgrade handshake). */
        if (!middleware_check_unlock(&req, client->ctx))
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
            client->body = NULL;
            client->body_len = 0;
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

static int parse_http(Client *client, const char *data, size_t len)
{
    const char *p = data;
    size_t remaining = len;

    if (!client->headers_done)
    {
        const char *header_end = strstr(p, "\r\n\r\n");
        if (!header_end) return 0;

        size_t header_len = header_end - p + 4;
        const char *path_start = NULL;
        const char *path_end = NULL;

        path_start = strchr(p, ' ');
        if (!path_start) return -1;
        path_start++;

        path_end = strchr(path_start, ' ');
        if (!path_end) return -1;

        size_t method_len = path_start - p - 1;
        if (method_len >= sizeof(client->method)) method_len = sizeof(client->method) - 1;
        memcpy(client->method, p, method_len);
        client->method[method_len] = '\0';

        size_t path_len = path_end - path_start;
        const char *qs = memchr(path_start, '?', path_len);
        if (qs)
        {
            size_t p_len = qs - path_start;
            if (p_len >= sizeof(client->path)) p_len = sizeof(client->path) - 1;
            memcpy(client->path, path_start, p_len);
            client->path[p_len] = '\0';
            size_t q_len = path_len - (qs - path_start) - 1;
            if (q_len >= sizeof(client->query)) q_len = sizeof(client->query) - 1;
            memcpy(client->query, qs + 1, q_len);
            client->query[q_len] = '\0';
        }
        else
        {
            if (path_len >= sizeof(client->path)) path_len = sizeof(client->path) - 1;
            memcpy(client->path, path_start, path_len);
            client->path[path_len] = '\0';
        }

        size_t header_copy_len = header_len;
        if (header_copy_len >= sizeof(client->headers)) header_copy_len = sizeof(client->headers) - 1;
        memcpy(client->headers, p, header_copy_len);
        client->headers[header_copy_len] = '\0';

        p = header_end + 4;
        remaining = len - (p - data);

        char headers_lower[4096];
        size_t hl = strlen(client->headers);
        if (hl >= sizeof(headers_lower)) hl = sizeof(headers_lower) - 1;
        for (size_t i = 0; i < hl; i++)
            headers_lower[i] = (client->headers[i] >= 'A' && client->headers[i] <= 'Z')
                               ? client->headers[i] + 32 : client->headers[i];
        headers_lower[hl] = '\0';

        const char *cl_str = strstr(headers_lower, "content-length:");
        client->content_length = 0;
        if (cl_str)
        {
            cl_str += 15;
            while (*cl_str == ' ') cl_str++;
            client->content_length = atoi(cl_str);
        }

        client->headers_done = 1;
    }

    if (remaining > 0 && client->content_length > 0)
    {
        size_t to_read = remaining;
        if (client->body_len + to_read > (size_t)client->content_length)
            to_read = (size_t)client->content_length - client->body_len;

        if (to_read > 0)
        {
            char *new_body = realloc(client->body, client->body_len + to_read + 1);
            if (!new_body) return -1;
            memcpy(new_body + client->body_len, p, to_read);
            client->body = new_body;
            client->body_len += to_read;
            client->body[client->body_len] = '\0';
        }
    }

    if (client->body_len >= (size_t)client->content_length)
        return 1;

    return 0;
}

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

    int rc = parse_http(client, buf->base, (size_t)nread);
    free(buf->base);

    if (rc < 0)
    {
        server_response_error(client, 400, "bad request");
        return;
    }

    if (rc == 1)
        handle_request(client);
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
