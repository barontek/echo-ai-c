/*
 * ollama.c - Ollama provider: chat, streaming chat, and structured
 * output against the local Ollama /api/chat endpoint.
 * Depends on: libcurl, cJSON, logging, string_utils, provider types.
 */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "provider.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

typedef struct {
    char *base_url;
    int num_ctx;
    int keep_alive_secs;
    char *effort; /* owned; NULL = model default ("low"/"medium"/"high"/"max"/"none") */
} OllamaCtx;

/* Process-global counter for synthetic tool-call ids (Ollama returns no
 * call ids of its own). Atomic so concurrent requests on different
 * threads never hand out the same id twice, which would break matching
 * a tool output back to its call id on later turns. */
static _Atomic unsigned int call_seq = 1;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int thinking_open;
    void (*on_chunk)(const char *, void *);
    void *userdata;
    ToolCall *tool_calls;
    int tool_calls_count;
    int tool_calls_cap;
} WriteBuf;

static void parse_stream_tool_calls(WriteBuf *buf, cJSON *msg)
{
    cJSON *tc_arr = cJSON_GetObjectItem(msg, "tool_calls");
    if (!tc_arr || !cJSON_IsArray(tc_arr)) return;

    int tc_count = cJSON_GetArraySize(tc_arr);
    for (int t = 0; t < tc_count; t++)
    {
        cJSON *tc = cJSON_GetArrayItem(tc_arr, t);
        cJSON *fn = cJSON_GetObjectItem(tc, "function");
        if (!fn) continue;
        cJSON *tname = cJSON_GetObjectItem(fn, "name");
        cJSON *args = cJSON_GetObjectItem(fn, "arguments");

        if (buf->tool_calls_count >= buf->tool_calls_cap)
        {
            int new_cap = buf->tool_calls_cap == 0 ? 4 : buf->tool_calls_cap * 2;
            ToolCall *new_tc = realloc(buf->tool_calls,
                                        sizeof(ToolCall) * (size_t)new_cap);
            if (new_tc)
            {
                memset(new_tc + buf->tool_calls_cap, 0,
                       sizeof(ToolCall) * (size_t)(new_cap - buf->tool_calls_cap));
                buf->tool_calls = new_tc;
                buf->tool_calls_cap = new_cap;
            }
        }

        if (buf->tool_calls_count < buf->tool_calls_cap)
        {
            ToolCall *dst = &buf->tool_calls[buf->tool_calls_count];
            dst->name = str_dup(tname && cJSON_IsString(tname)
                                  ? cJSON_GetStringValue(tname) : "");
            dst->id = str_dup("");
            if (args)
            {
                char *args_str = cJSON_PrintUnformatted(args);
                dst->arguments = args_str ? args_str : str_dup("");
            }
            else
            {
                dst->arguments = str_dup("");
            }
            if (dst->name && dst->id && dst->arguments && dst->name[0])
                buf->tool_calls_count++;
            else
            {
                free(dst->name);
                free(dst->id);
                free(dst->arguments);
                memset(dst, 0, sizeof(*dst));
            }
        }
    }
}

static void forward_chunk(WriteBuf *buf, cJSON *msg)
{
    cJSON *thinking = cJSON_GetObjectItem(msg, "thinking");
    if (thinking && cJSON_IsString(thinking) && strlen(cJSON_GetStringValue(thinking)) > 0)
    {
        if (!buf->thinking_open)
        {
            buf->on_chunk("<think>\n", buf->userdata);
            buf->thinking_open = 1;
        }
        buf->on_chunk(cJSON_GetStringValue(thinking), buf->userdata);
    }

    cJSON *content = cJSON_GetObjectItem(msg, "content");
    if (content && cJSON_IsString(content) && strlen(cJSON_GetStringValue(content)) > 0)
    {
        if (buf->thinking_open)
        {
            buf->on_chunk("\n</think>\n\n", buf->userdata);
            buf->thinking_open = 0;
        }
        buf->on_chunk(cJSON_GetStringValue(content), buf->userdata);
    }
}

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
                        if (msg)
                        {
                            forward_chunk(buf, msg);
                            parse_stream_tool_calls(buf, msg);
                        }
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
                                 void *userdata,
                                 ToolCall **out_tool_calls,
                                 int *out_tool_calls_count)
{
    (void)timeout;
    /* The timeout argument is deliberately ignored: CURLOPT_TIMEOUT 0
     * lets reasoning models think for as long as they need, bounded
     * only by the 60s no-progress cutoff below. */
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);

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
        if (buf.tool_calls)
        {
            for (int i = 0; i < buf.tool_calls_count; i++)
                tool_call_free(&buf.tool_calls[i]);
            free(buf.tool_calls);
        }
        free(buf.data);
        return NULL;
    }

    if (stream && on_chunk)
    {
        /* The stream usually ends without a trailing newline, so the
         * final JSON line is still sitting in the buffer: parse it
         * here to deliver the last chunk and tool calls. */
        if (buf.len > 0 && buf.data[0] != '\0')
        {
            cJSON *json = cJSON_Parse(buf.data);
            if (json)
            {
                cJSON *msg = cJSON_GetObjectItem(json, "message");
                if (msg) forward_chunk(&buf, msg);
                cJSON_Delete(json);
            }
        }
        if (out_tool_calls && out_tool_calls_count)
        {
            *out_tool_calls = buf.tool_calls;
            *out_tool_calls_count = buf.tool_calls_count;
        }
        free(buf.data);
        return str_dup("");
    }
    /* non-streaming: caller frees the body; tool_calls in buf are never populated */
    if (buf.tool_calls)
    {
        for (int i = 0; i < buf.tool_calls_count; i++)
            tool_call_free(&buf.tool_calls[i]);
        free(buf.tool_calls);
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
    cJSON *thinking = cJSON_GetObjectItem(msg, "thinking");

    resp->thinking = NULL;
    if (thinking && cJSON_IsString(thinking) && strlen(cJSON_GetStringValue(thinking)) > 0)
    {
        resp->thinking = str_dup(cJSON_GetStringValue(thinking));
        const char *ct_str = content && cJSON_IsString(content) ? cJSON_GetStringValue(content) : "";
        if (asprintf(&resp->content, "<think>\n%s\n</think>\n\n%s",
                     cJSON_GetStringValue(thinking), ct_str) < 0)
            resp->content = NULL;
    }
    else if (content && cJSON_IsString(content))
    {
        resp->content = str_dup(cJSON_GetStringValue(content));
    }

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
                    {
                        char id_buf[32];
                        snprintf(id_buf, sizeof(id_buf), "call_%u",
                                 atomic_fetch_add_explicit(&call_seq, 1,
                                                           memory_order_relaxed));
                        resp->tool_calls[i].id = str_dup(id_buf);
                    }

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

/* Ollama passes reasoning_effort through its options map for reasoning
 * models; the accepted levels match openai_compatible (no xhigh). */
int ollama_reasoning_effort_valid(const char *effort)
{
    if (!effort || !effort[0]) return 1;
    return strcmp(effort, "low") == 0 ||
           strcmp(effort, "medium") == 0 ||
           strcmp(effort, "high") == 0 ||
           strcmp(effort, "max") == 0 ||
           strcmp(effort, "none") == 0;
}

/* Renders the "reasoning_effort" entry (with trailing comma) for the
 * options map, or an empty string when no effort is set. Returns -1 on
 * invalid effort or a buffer too small for the value. */
static int reasoning_effort_fragment(const char *effort, char *buf, size_t cap)
{
    if (!effort || !effort[0])
    {
        buf[0] = '\0';
        return 0;
    }
    if (!ollama_reasoning_effort_valid(effort)) return -1;
    if (snprintf(buf, cap, "\"reasoning_effort\":\"%s\",", effort) >= (int)cap)
        return -1;
    return 0;
}

static LLMResponse *ollama_chat(LLMProvider *self, Message *messages, int count,
                                const char *model, double temperature, int timeout,
                                const char *tools_json)
{
    OllamaCtx *ctx = self->ctx;

    char *msgs_json = llm_messages_format(messages, count, NULL, NULL);
    if (!msgs_json) return NULL;

    char effort_frag[64];
    if (reasoning_effort_fragment(ctx->effort, effort_frag,
                                  sizeof(effort_frag)) != 0)
    {
        free(msgs_json);
        return NULL;
    }

    char *body = NULL;
    if (tools_json && tools_json[0])
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":false,"
                     "\"options\":{%s\"temperature\":%.2f,\"num_ctx\":%d},"
                     "\"keep_alive\":%d,\"tools\":%s}",
                     model, msgs_json, effort_frag, temperature, ctx->num_ctx,
                     ctx->keep_alive_secs, tools_json) < 0)
        {
            free(msgs_json);
            return NULL;
        }
    }
    else
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":false,"
                     "\"options\":{%s\"temperature\":%.2f,\"num_ctx\":%d},"
                     "\"keep_alive\":%d}",
                     model, msgs_json, effort_frag, temperature, ctx->num_ctx,
                     ctx->keep_alive_secs) < 0)
        {
            free(msgs_json);
            return NULL;
        }
    }
    free(msgs_json);

    log_debug("ollama request", "model", model, NULL);

    char *raw = ollama_chat_request(ctx->base_url, body, timeout, 0, NULL, NULL, NULL, NULL);
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

    char effort_frag[64];
    if (reasoning_effort_fragment(ctx->effort, effort_frag,
                                  sizeof(effort_frag)) != 0)
    {
        free(msgs_json);
        return NULL;
    }

    char *body = NULL;
    if (tools_json && tools_json[0])
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":true,"
                     "\"options\":{%s\"temperature\":%.2f,\"num_ctx\":%d},"
                     "\"keep_alive\":%d,\"tools\":%s}",
                     model, msgs_json, effort_frag, temperature, ctx->num_ctx,
                     ctx->keep_alive_secs, tools_json) < 0)
        {
            free(msgs_json);
            return NULL;
        }
    }
    else
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":true,"
                     "\"options\":{%s\"temperature\":%.2f,\"num_ctx\":%d},"
                     "\"keep_alive\":%d}",
                     model, msgs_json, effort_frag, temperature, ctx->num_ctx,
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

    ToolCall *tc_from_stream = NULL;
    int tc_count = 0;
    char *raw = ollama_chat_request(ctx->base_url, body, timeout, 1,
                                    on_ollama_chunk, &sctx,
                                    &tc_from_stream, &tc_count);
    free(body);

    if (!raw)
    {
        if (tc_from_stream)
        {
            for (int i = 0; i < tc_count; i++)
                tool_call_free(&tc_from_stream[i]);
            free(tc_from_stream);
        }
        llm_response_free(sctx.resp);
        return NULL;
    }

    if (tc_count > 0)
    {
        sctx.resp->tool_calls = tc_from_stream;
        sctx.resp->tool_calls_count = tc_count;
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

    char effort_frag[64];
    if (reasoning_effort_fragment(ctx->effort, effort_frag,
                                  sizeof(effort_frag)) != 0)
    {
        free(msgs_json);
        return NULL;
    }

    char *body = NULL;
    if (json_schema && json_schema[0])
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":false,"
                     "\"options\":{%s\"temperature\":%.2f,\"num_ctx\":%d},"
                     "\"keep_alive\":%d,"
                     "\"format\":%s}",
                     model, msgs_json, effort_frag, temperature, ctx->num_ctx,
                     ctx->keep_alive_secs, json_schema) < 0)
        { free(msgs_json); return NULL; }
    }
    else
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":false,"
                     "\"options\":{%s\"temperature\":%.2f,\"num_ctx\":%d},"
                     "\"keep_alive\":%d,"
                     "\"format\":\"json\"}",
                     model, msgs_json, effort_frag, temperature, ctx->num_ctx,
                     ctx->keep_alive_secs) < 0)
        { free(msgs_json); return NULL; }
    }
    free(msgs_json);

    char *raw = ollama_chat_request(ctx->base_url, body, timeout, 0, NULL, NULL, NULL, NULL);
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
    free(ctx->effort);
    free(ctx);
    free(self);
}

LLMProvider *ollama_provider_create(const char *base_url, int num_ctx,
                                    int keep_alive_secs, const char *effort)
{
    if (effort && !ollama_reasoning_effort_valid(effort))
    {
        log_error("Ollama provider rejected invalid reasoning effort",
                  "effort", effort, NULL);
        return NULL;
    }

    LLMProvider *p = calloc(1, sizeof(LLMProvider));
    if (!p) return NULL;

    OllamaCtx *ctx = calloc(1, sizeof(OllamaCtx));
    if (!ctx) { free(p); return NULL; }

    ctx->base_url = str_dup(base_url ? base_url : "http://localhost:11434");
    if (!ctx->base_url) { free(ctx); free(p); return NULL; }
    ctx->num_ctx = num_ctx > 0 ? num_ctx : 4096;
    ctx->keep_alive_secs = keep_alive_secs > 0 ? keep_alive_secs : 120;

    if (effort && effort[0])
    {
        ctx->effort = str_dup(effort);
        if (!ctx->effort) { free(ctx->base_url); free(ctx); free(p); return NULL; }
    }

    p->chat = ollama_chat;
    p->chat_streaming = ollama_chat_streaming;
    p->extract_structured = ollama_extract_structured;
    p->destroy = ollama_destroy;
    p->ctx = ctx;
    return p;
}

#ifdef OLLAMA_TEST
/* ---- Captured values for testing (set by curl stubs below) ---- */
char *ollama_test_captured_url = NULL;
char *ollama_test_captured_body = NULL;
int ollama_test_curl_init_fails = 0;
CURLcode ollama_test_curl_result = CURLE_OK;

void ollama_test_parse_stream_tool_calls(WriteBuf *buf, cJSON *msg)
{
    parse_stream_tool_calls(buf, msg);
}

void ollama_test_forward_chunk(WriteBuf *buf, cJSON *msg)
{
    forward_chunk(buf, msg);
}

LLMResponse *ollama_test_parse_response(const char *raw)
{
    return ollama_parse_response(raw);
}

size_t ollama_test_write_cb(const char *data, size_t len, void *userdata)
{
    return write_cb((void *)data, 1, len, userdata);
}

char *ollama_test_build_url(const char *base_url)
{
    return build_url(base_url);
}

/* ---- curl stub implementations ---- */
#undef curl_easy_setopt
#undef curl_easy_init
#undef curl_easy_perform
#undef curl_easy_cleanup
#undef curl_easy_strerror
#undef curl_slist_append
#undef curl_slist_free_all

CURL *curl_easy_init(void)
{
    if (ollama_test_curl_init_fails) return NULL;
    static int dummy;
    return (CURL *)&dummy;
}

CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...)
{
    (void)curl;
    va_list args;
    va_start(args, option);
    if (option == CURLOPT_URL)
        ollama_test_captured_url = va_arg(args, char *);
    else if (option == CURLOPT_POSTFIELDS)
    {
        free(ollama_test_captured_body);
        ollama_test_captured_body = str_dup(va_arg(args, char *));
    }
    else if (option == CURLOPT_POST)
        (void)va_arg(args, long);
    else if (option == CURLOPT_HTTPHEADER)
        (void)va_arg(args, struct curl_slist *);
    else if (option == CURLOPT_WRITEFUNCTION)
        (void)va_arg(args, void *);
    else if (option == CURLOPT_WRITEDATA)
        (void)va_arg(args, void *);
    else if (option == CURLOPT_TIMEOUT)
        (void)va_arg(args, long);
    else if (option == CURLOPT_LOW_SPEED_LIMIT)
        (void)va_arg(args, long);
    else if (option == CURLOPT_LOW_SPEED_TIME)
        (void)va_arg(args, long);
    else
        (void)va_arg(args, void *);
    va_end(args);
    return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *curl)
{
    (void)curl;
    return ollama_test_curl_result;
}

void curl_easy_cleanup(CURL *curl)
{
    (void)curl;
}

const char *curl_easy_strerror(CURLcode code)
{
    (void)code;
    return "stub error";
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data)
{
    (void)list;
    (void)data;
    static struct curl_slist dummy;
    return &dummy;
}

void curl_slist_free_all(struct curl_slist *list)
{
    (void)list;
}
#endif
