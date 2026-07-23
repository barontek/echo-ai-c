#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
} WebCtx;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} WriteBuf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    WriteBuf *buf = userdata;
    size_t total = size * nmemb;
    size_t needed = buf->len + total + 1;
    if (needed > buf->cap)
    {
        buf->cap = needed * 2;
        char *new = realloc(buf->data, buf->cap);
        if (!new) return 0;
        buf->data = new;
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

    const char *url = cJSON_GetStringValue(url_json);
    cJSON_Delete(args);

    if (!safety_check_url(ctx->safety, url))
        return tool_result_error("URL rejected by network policy", "policy_denied");

    CURL *curl = curl_easy_init();
    if (!curl) return tool_result_error("curl init failed", "execution_error");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "EchoAI/1.0");

    WriteBuf buf = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        free(buf.data);
        char *err = NULL;
        if (asprintf(&err, "HTTP request failed: %s", curl_easy_strerror(res)) < 0)
            err = str_dup("HTTP request failed");
        ToolResult *tr = tool_result_error(err, "execution_error");
        free(err);
        return tr;
    }

    if (buf.len > ctx->safety->max_file_size)
    {
        free(buf.data);
        return tool_result_error("response too large", "policy_denied");
    }

    ToolResult *tr = tool_result_create(buf.data ? buf.data : "(empty response)");
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
    t->description = str_dup("Fetch a URL and return its text content");
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
