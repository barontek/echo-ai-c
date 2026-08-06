#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <curl/curl.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/html_extract.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
} WebCtx;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    size_t max_len;
    int too_large;
} WriteBuf;

static curl_socket_t open_socket_cb(void *userdata, curlsocktype purpose,
                                    struct curl_sockaddr *address)
{
    (void)userdata;
    (void)purpose;
    if (!safety_check_socket_address((const struct sockaddr *)&address->addr))
        return CURL_SOCKET_BAD;
    return socket(address->family, address->socktype, address->protocol);
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    WriteBuf *buf = userdata;
    if (size != 0 && nmemb > SIZE_MAX / size) return 0;
    size_t total = size * nmemb;
    if (total > buf->max_len - buf->len)
    {
        buf->too_large = 1;
        return 0;
    }
    if (total > SIZE_MAX - buf->len - 1) return 0;
    size_t needed = buf->len + total + 1;
    if (needed > buf->cap)
    {
        size_t new_cap = needed > SIZE_MAX / 2 ? needed : needed * 2;
        char *new = realloc(buf->data, new_cap);
        if (!new) return 0;
        buf->data = new;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static ToolResult *web_fetch_execute(Tool *self, const char *args_json)
{
    WebCtx *ctx = self->ctx;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *url_json = cJSON_GetObjectItem(args, "url");
    if (!url_json || !cJSON_IsString(url_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'url' argument", "validation_error");
    }

    char *url = str_dup(cJSON_GetStringValue(url_json));
    cJSON_Delete(args);

    if (!safety_check_url(ctx->safety, url))
    {
        free(url);
        return tool_result_error("URL rejected by network policy", "policy_denied");
    }

    CURL *curl = curl_easy_init();
    if (!curl) { free(url); return tool_result_error("curl init failed", "execution_error"); }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    free(url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    /* Bot-detection hygiene: HTTP/2 and a browser-shaped header set. A bare
     * UA (e.g. "EchoAI/1.0") plus HTTP/1.1 gets 403'd by Cloudflare/WAF
     * filters even on pages that allow automated access. */
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8");
    hdrs = curl_slist_append(hdrs, "Accept-Language: en-US,en;q=0.9");
    hdrs = curl_slist_append(hdrs, "Upgrade-Insecure-Requests: 1");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Dest: document");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Mode: navigate");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Site: none");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-User: ?1");
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    /* Accept-Encoding is negotiated by curl itself ("" = all supported
     * encodings); without this the server's gzip/br response arrives
     * undecoded and the extractor sees compressed bytes. */
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, open_socket_cb);
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, ctx->safety);

    WriteBuf buf = {.max_len = ctx->safety->max_file_size};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);

    /* Capture the Content-Type before cleanup: it decides whether the body
     * is extracted (HTML), truncated (text) or replaced by a descriptor. */
    char *ctype = NULL;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ctype);
    char *ctype_copy = ctype ? str_dup(ctype) : NULL;
    curl_easy_cleanup(curl);
    curl_slist_free_all(hdrs);

    if (res != CURLE_OK)
    {
        free(buf.data);
        free(ctype_copy);
        if (buf.too_large)
            return tool_result_error("response too large", "policy_denied");
        char *err = NULL;
        if (asprintf(&err, "HTTP request failed: %s", curl_easy_strerror(res)) < 0)
            err = str_dup("HTTP request failed");
        ToolResult *tr = tool_result_error(err, "execution_error");
        free(err);
        return tr;
    }

    char *simplified = content_extract_for_llm(ctype_copy, buf.data, buf.len,
                                               ctx->safety->web_fetch_max_chars);
    free(ctype_copy);
    if (!simplified)
    {
        free(buf.data);
        return tool_result_error("out of memory during content extraction",
                                 "execution_error");
    }

    ToolResult *tr = tool_result_create(simplified);
    free(simplified);
    free(buf.data);
    return tr;
}

static void web_fetch_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

Tool *tool_web_fetch_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    WebCtx *ctx = calloc(1, sizeof(WebCtx));
    if (!ctx) { free(t); return NULL; }
    ctx->safety = safety;

    t->name = str_dup("web_fetch");
    t->description = str_dup("Fetch a URL and return its extracted text "
                             "(HTML boilerplate stripped, links as citations)");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"url\":{\"type\":\"string\",\"description\":\"URL to fetch\"}"
        "},\"required\":[\"url\"]}"
    );
    t->execute = web_fetch_execute;
    t->destroy = web_fetch_destroy;
    t->ctx = ctx;
    return t;
}
