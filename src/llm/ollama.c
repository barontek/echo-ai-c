#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "provider.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

typedef struct {
    char *base_url;
    int num_ctx;
    int keep_alive_secs;
} OllamaCtx;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    void (*on_chunk)(const char *, void *);
    void *userdata;
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

    if (buf->on_chunk)
    {
        char *line_start = buf->data;
        char *p = buf->data;
        while (p < buf->data + buf->len)
        {
            if (*p == '\n')
            {
                *p = '\0';
                if (line_start[0] != '\0')
                {
                    cJSON *json = cJSON_Parse(line_start);
                    if (json)
                    {
                        cJSON *msg = cJSON_GetObjectItem(json, "message");
                        cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
                        if (content && cJSON_IsString(content))
                            buf->on_chunk(cJSON_GetStringValue(content), buf->userdata);
                        cJSON_Delete(json);
                    }
                }
                line_start = p + 1;
            }
            p++;
        }
        if (line_start > buf->data)
        {
            size_t remaining = buf->len - (size_t)(line_start - buf->data);
            if (remaining > 0)
                memmove(buf->data, line_start, remaining);
            buf->len = remaining;
            buf->data[buf->len] = '\0';
        }
    }

    return total;
}

static char *build_url(const char *base_url)
{
    char *url = NULL;
    if (asprintf(&url, "%s/api/chat", base_url) < 0) return NULL;
    return url;
}

static char *ollama_chat_request(const char *base_url, const char *json_body,
                                 int timeout, int stream,
                                 void (*on_chunk)(const char *, void *),
                                 void *userdata)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    char *url = build_url(base_url);
    if (!url) { curl_easy_cleanup(curl); return NULL; }

    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    if (!headers) { free(url); curl_easy_cleanup(curl); return NULL; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);

    WriteBuf buf = {0};
    if (stream && on_chunk)
    {
        buf.on_chunk = on_chunk;
        buf.userdata = userdata;
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    free(url);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        log_error("ollama request failed", "error", curl_easy_strerror(res), NULL);
        free(buf.data);
        return NULL;
    }

    if (stream && on_chunk)
    {
        if (buf.len > 0 && buf.data[0] != '\0')
        {
            cJSON *json = cJSON_Parse(buf.data);
            if (json)
            {
                cJSON *msg = cJSON_GetObjectItem(json, "message");
                cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
                if (content && cJSON_IsString(content))
                    on_chunk(cJSON_GetStringValue(content), userdata);
                cJSON_Delete(json);
            }
        }
        free(buf.data);
        return str_dup("");
    }

    return buf.data;
}

static LLMResponse *ollama_parse_response(const char *raw)
{
    LLMResponse *resp = llm_response_create();
    if (!resp) return NULL;

    cJSON *json = cJSON_Parse(raw);
    if (!json)
    {
        log_error("ollama parse failed", "response", raw, NULL);
        free(resp);
        return NULL;
    }

    cJSON *msg = cJSON_GetObjectItem(json, "message");
    if (!msg)
    {
        cJSON_Delete(json);
        free(resp);
        return NULL;
    }

    cJSON *content = cJSON_GetObjectItem(msg, "content");
    if (content && cJSON_IsString(content))
        resp->content = str_dup(cJSON_GetStringValue(content));

    cJSON *tc_arr = cJSON_GetObjectItem(msg, "tool_calls");
    if (tc_arr && cJSON_IsArray(tc_arr))
    {
        int count = cJSON_GetArraySize(tc_arr);
        if (count > 0)
        {
            resp->tool_calls = calloc(count, sizeof(ToolCall));
            if (resp->tool_calls)
            {
                resp->tool_calls_count = count;
                for (int i = 0; i < count; i++)
                {
                    cJSON *tc = cJSON_GetArrayItem(tc_arr, i);
                    cJSON *fn = cJSON_GetObjectItem(tc, "function");
                    if (!fn) continue;

                    cJSON *name = cJSON_GetObjectItem(fn, "name");
                    cJSON *args = cJSON_GetObjectItem(fn, "arguments");

                    resp->tool_calls[i].name = str_dup(name && cJSON_IsString(name)
                        ? cJSON_GetStringValue(name) : "");
                    resp->tool_calls[i].id = str_dup("");

                    if (args)
                    {
                        char *args_str = cJSON_PrintUnformatted(args);
                        resp->tool_calls[i].arguments = args_str ? args_str : str_dup("");
                    }
                    else
                    {
                        resp->tool_calls[i].arguments = str_dup("");
                    }
                }
            }
        }
    }

    cJSON_Delete(json);
    return resp;
}

static LLMResponse *ollama_chat(LLMProvider *self, Message *messages, int count,
                                const char *model, double temperature, int timeout,
                                const char *tools_json)
{
    OllamaCtx *ctx = self->ctx;

    char *msgs_json = llm_messages_format(messages, count, NULL, NULL);
    if (!msgs_json) return NULL;

    char *body = NULL;
    if (tools_json && tools_json[0])
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":false,"
                     "\"options\":{\"temperature\":%.2f,\"num_ctx\":%d},"
                     "\"keep_alive\":%d,\"tools\":%s}",
                     model, msgs_json, temperature, ctx->num_ctx,
                     ctx->keep_alive_secs, tools_json) < 0)
        {
            free(msgs_json);
            return NULL;
        }
    }
    else
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":false,"
                     "\"options\":{\"temperature\":%.2f,\"num_ctx\":%d},"
                     "\"keep_alive\":%d}",
                     model, msgs_json, temperature, ctx->num_ctx,
                     ctx->keep_alive_secs) < 0)
        {
            free(msgs_json);
            return NULL;
        }
    }
    free(msgs_json);

    log_debug("ollama request", "model", model, NULL);

    char *raw = ollama_chat_request(ctx->base_url, body, timeout, 0, NULL, NULL);
    free(body);

    if (!raw) return NULL;

    LLMResponse *resp = ollama_parse_response(raw);
    free(raw);
    return resp;
}

typedef struct {
    LLMResponse *resp;
    void (*on_chunk)(const char *, void *);
    void *userdata;
} StreamCtx;

static void on_ollama_chunk(const char *chunk, void *userdata)
{
    StreamCtx *sctx = userdata;
    if (!sctx->resp->content)
    {
        sctx->resp->content = str_dup(chunk);
    }
    else
    {
        size_t old = strlen(sctx->resp->content);
        size_t clen = strlen(chunk);
        char *new = realloc(sctx->resp->content, old + clen + 1);
        if (new)
        {
            sctx->resp->content = new;
            memcpy(new + old, chunk, clen + 1);
        }
    }
    if (sctx->on_chunk)
        sctx->on_chunk(chunk, sctx->userdata);
}

static LLMResponse *ollama_chat_streaming(LLMProvider *self, Message *messages, int count,
                                          const char *model, double temperature, int timeout,
                                          void (*on_chunk)(const char *, void *),
                                          void *userdata,
                                          const char *tools_json)
{
    OllamaCtx *ctx = self->ctx;

    char *msgs_json = llm_messages_format(messages, count, NULL, NULL);
    if (!msgs_json) return NULL;

    char *body = NULL;
    if (tools_json && tools_json[0])
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":true,"
                     "\"options\":{\"temperature\":%.2f,\"num_ctx\":%d},"
                     "\"keep_alive\":%d,\"tools\":%s}",
                     model, msgs_json, temperature, ctx->num_ctx,
                     ctx->keep_alive_secs, tools_json) < 0)
        {
            free(msgs_json);
            return NULL;
        }
    }
    else
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":true,"
                     "\"options\":{\"temperature\":%.2f,\"num_ctx\":%d},"
                     "\"keep_alive\":%d}",
                     model, msgs_json, temperature, ctx->num_ctx,
                     ctx->keep_alive_secs) < 0)
        {
            free(msgs_json);
            return NULL;
        }
    }
    free(msgs_json);

    StreamCtx sctx = {0};
    sctx.on_chunk = on_chunk;
    sctx.userdata = userdata;
    sctx.resp = llm_response_create();
    if (!sctx.resp) { free(body); return NULL; }

    char *raw = ollama_chat_request(ctx->base_url, body, timeout, 1,
                                    on_ollama_chunk, &sctx);
    free(body);

    if (!raw)
    {
        llm_response_free(sctx.resp);
        return NULL;
    }

    free(raw);
    return sctx.resp;
}

static LLMResponse *ollama_extract_structured(LLMProvider *self, Message *messages, int count,
                                               const char *model, double temperature, int timeout,
                                               const char *json_schema)
{
    OllamaCtx *ctx = self->ctx;

    char *msgs_json = llm_messages_format(messages, count, NULL, NULL);
    if (!msgs_json) return NULL;

    char *body = NULL;
    if (json_schema && json_schema[0])
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":false,"
                     "\"options\":{\"temperature\":%.2f,\"num_ctx\":%d},"
                     "\"keep_alive\":%d,"
                     "\"format\":%s}",
                     model, msgs_json, temperature, ctx->num_ctx,
                     ctx->keep_alive_secs, json_schema) < 0)
        { free(msgs_json); return NULL; }
    }
    else
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":false,"
                     "\"options\":{\"temperature\":%.2f,\"num_ctx\":%d},"
                     "\"keep_alive\":%d,"
                     "\"format\":\"json\"}",
                     model, msgs_json, temperature, ctx->num_ctx,
                     ctx->keep_alive_secs) < 0)
        { free(msgs_json); return NULL; }
    }
    free(msgs_json);

    char *raw = ollama_chat_request(ctx->base_url, body, timeout, 0, NULL, NULL);
    free(body);

    if (!raw) return NULL;

    LLMResponse *resp = ollama_parse_response(raw);
    free(raw);
    return resp;
}

static void ollama_destroy(LLMProvider *self)
{
    if (!self) return;
    OllamaCtx *ctx = self->ctx;
    free(ctx->base_url);
    free(ctx);
    free(self);
}

LLMProvider *ollama_provider_create(const char *base_url, int num_ctx, int keep_alive_secs)
{
    LLMProvider *p = calloc(1, sizeof(LLMProvider));
    if (!p) return NULL;

    OllamaCtx *ctx = calloc(1, sizeof(OllamaCtx));
    if (!ctx) { free(p); return NULL; }

    ctx->base_url = str_dup(base_url ? base_url : "http://localhost:11434");
    ctx->num_ctx = num_ctx > 0 ? num_ctx : 4096;
    ctx->keep_alive_secs = keep_alive_secs > 0 ? keep_alive_secs : 120;

    p->chat = ollama_chat;
    p->chat_streaming = ollama_chat_streaming;
    p->extract_structured = ollama_extract_structured;
    p->destroy = ollama_destroy;
    p->ctx = ctx;
    return p;
}
