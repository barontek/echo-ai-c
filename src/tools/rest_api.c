/*
 * rest_api.c - generic REST HTTP tool: issues GET/POST/PUT/DELETE/PATCH
 * requests against user-supplied URLs, enforcing the network policy's
 * socket rules. Depends on: tool.h, libcurl, safety, string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <curl/curl.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/http_client.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
} RestCtx;

static curl_socket_t open_socket_cb(void *userdata, curlsocktype purpose,
                                    struct curl_sockaddr *address)
{
    (void)userdata;
    (void)purpose;
    if (!safety_check_socket_address((const struct sockaddr *)&address->addr))
        return CURL_SOCKET_BAD;
    return socket(address->family, address->socktype, address->protocol);
}

static ToolResult *rest_api_execute(Tool *self, const char *args_json)
{
    RestCtx *ctx = self->ctx;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *url_json = cJSON_GetObjectItem(args, "url");
    cJSON *method_json = cJSON_GetObjectItem(args, "method");
    cJSON *body_json = cJSON_GetObjectItem(args, "body");
    cJSON *headers_json = cJSON_GetObjectItem(args, "headers");

    if (!url_json || !cJSON_IsString(url_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'url' argument", "validation_error");
    }

    char *url = str_dup(cJSON_GetStringValue(url_json));
    char *method = str_dup(method_json && cJSON_IsString(method_json)
                           ? cJSON_GetStringValue(method_json) : "GET");

    if (!url || !method)
    {
        free(url); free(method);
        cJSON_Delete(args);
        return tool_result_error("oom", "execution_error");
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(NULL, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    if (headers_json && cJSON_IsObject(headers_json))
    {
        cJSON *hdr = NULL;
        cJSON_ArrayForEach(hdr, headers_json)
        {
            if (hdr->valuestring)
            {
                char *h = NULL;
                if (asprintf(&h, "%s: %s", hdr->string, hdr->valuestring) >= 0)
                {
                    headers = curl_slist_append(headers, h);
                    free(h);
                }
            }
        }
    }

    char *body_str = NULL;
    if (body_json)
    {
        if (cJSON_IsString(body_json))
            body_str = str_dup(cJSON_GetStringValue(body_json));
        else
            body_str = cJSON_PrintUnformatted(body_json);
    }

    cJSON_Delete(args);

    if (!safety_check_url(ctx->safety, url))
    {
        free(url); free(method); free(body_str);
        curl_slist_free_all(headers);
        return tool_result_error("URL rejected by network policy", "policy_denied");
    }

    CURL *curl = curl_easy_init();
    if (!curl)
    {
        free(url); free(method); free(body_str);
        curl_slist_free_all(headers);
        return tool_result_error("curl init failed", "execution_error");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "EchoAI/1.0");
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, open_socket_cb);
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, ctx->safety);

    if (strcmp(method, "POST") == 0)
    {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (body_str)
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
    }
    else if (strcmp(method, "PUT") == 0)
    {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (body_str)
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
    }
    else if (strcmp(method, "DELETE") == 0)
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    else if (strcmp(method, "PATCH") == 0)
    {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        if (body_str)
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
    }

    HttpBuffer buf = {.limit = ctx->safety->max_file_size};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_buffer_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    long http_code = 0;
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(url);
    free(method);
    free(body_str);

    if (res != CURLE_OK)
    {
        free(buf.data);
        if (buf.too_large)
            return tool_result_error("response too large", "policy_denied");
        char *err = NULL;
        if (asprintf(&err, "HTTP request failed: %s", curl_easy_strerror(res)) < 0)
            err = str_dup("HTTP request failed");
        ToolResult *tr = tool_result_error(err, "execution_error");
        free(err);
        return tr;
    }

    char *result = NULL;
    if (asprintf(&result, "HTTP %ld\n\n%s", http_code,
                 buf.data ? buf.data : "(empty response)") < 0)
        result = str_dup("(no output)");

    ToolResult *tr = tool_result_create(result);
    free(result);
    free(buf.data);
    return tr;
}

static void rest_api_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_rest_api_create - construct the rest_api tool
 * @safety: borrowed SafetyConfig consulted on every execution; not owned
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
Tool *tool_rest_api_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    RestCtx *ctx = calloc(1, sizeof(RestCtx));
    if (!ctx) {
        free(t);
        return NULL;
    }
    ctx->safety = safety;

    t->name = str_dup("rest_api");
    t->description = str_dup("Make HTTP requests (GET, POST, PUT, DELETE, PATCH)");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"url\":{\"type\":\"string\",\"description\":\"Request URL\"},"
        "\"method\":{\"type\":\"string\",\"enum\":[\"GET\",\"POST\",\"PUT\",\"DELETE\",\"PATCH\"],\"description\":\"HTTP method (default GET)\"},"
        "\"body\":{\"type\":\"string\",\"description\":\"Request body (JSON string)\"},"
        "\"headers\":{\"type\":\"object\",\"description\":\"Additional headers\"}"
        "},\"required\":[\"url\"]}"
    );
    t->execute = rest_api_execute;
    t->destroy = rest_api_destroy;
    t->ctx = ctx;
    return t;
}
