#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "openai_compatible.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

typedef struct {
    char *base_url;
    char *api_token;
    char *effort; /* owned; NULL = API default ("low"/"medium"/"high"/"max"/"none") */
} OpenAICompatCtx;

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

static int append_text(char **dst, const char *text)
{
    if (!text) return 0;
    size_t old_len = *dst ? strlen(*dst) : 0;
    size_t add_len = strlen(text);
    if (add_len > SIZE_MAX - old_len - 1) return -1;
    char *new_value = realloc(*dst, old_len + add_len + 1);
    if (!new_value) return -1;
    memcpy(new_value + old_len, text, add_len + 1);
    *dst = new_value;
    return 0;
}

static int ensure_tool_call(LLMResponse *resp, int index)
{
    if (index < 0) return -1;
    if (index < resp->tool_calls_count) return 0;
    size_t new_count = (size_t)index + 1;
    if (new_count > SIZE_MAX / sizeof(ToolCall)) return -1;
    ToolCall *new_calls = realloc(resp->tool_calls, new_count * sizeof(ToolCall));
    if (!new_calls) return -1;
    memset(new_calls + resp->tool_calls_count, 0,
           (new_count - (size_t)resp->tool_calls_count) * sizeof(ToolCall));
    resp->tool_calls = new_calls;
    resp->tool_calls_count = index + 1;
    return 0;
}

static int parse_stream_event(LLMResponse *resp, const char *json_text,
                              void (*on_chunk)(const char *, void *),
                              void *userdata)
{
    cJSON *json = cJSON_Parse(json_text);
    if (!json) return -1;
    cJSON *choices = cJSON_GetObjectItem(json, "choices");
    cJSON *choice = choices && cJSON_IsArray(choices)
                        ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *delta = choice ? cJSON_GetObjectItem(choice, "delta") : NULL;
    if (!delta)
    {
        cJSON_Delete(json);
        return 0;
    }

    cJSON *content = cJSON_GetObjectItem(delta, "content");
    if (content && cJSON_IsString(content))
    {
        const char *chunk = cJSON_GetStringValue(content);
        if (append_text(&resp->content, chunk) != 0)
        {
            cJSON_Delete(json);
            return -1;
        }
        if (on_chunk) on_chunk(chunk, userdata);
    }

    cJSON *tool_calls = cJSON_GetObjectItem(delta, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls))
    {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, tool_calls)
        {
            cJSON *index_json = cJSON_GetObjectItem(item, "index");
            int index = index_json && cJSON_IsNumber(index_json)
                            ? index_json->valueint : 0;
            if (ensure_tool_call(resp, index) != 0)
            {
                cJSON_Delete(json);
                return -1;
            }
            ToolCall *call = &resp->tool_calls[index];
            cJSON *id = cJSON_GetObjectItem(item, "id");
            cJSON *function = cJSON_GetObjectItem(item, "function");
            cJSON *name = function ? cJSON_GetObjectItem(function, "name") : NULL;
            cJSON *arguments = function ? cJSON_GetObjectItem(function, "arguments") : NULL;
            if ((id && cJSON_IsString(id) &&
                 append_text(&call->id, cJSON_GetStringValue(id)) != 0) ||
                (name && cJSON_IsString(name) &&
                 append_text(&call->name, cJSON_GetStringValue(name)) != 0) ||
                (arguments && cJSON_IsString(arguments) &&
                 append_text(&call->arguments, cJSON_GetStringValue(arguments)) != 0))
            {
                cJSON_Delete(json);
                return -1;
            }
        }
    }

    cJSON_Delete(json);
    return 0;
}

typedef struct {
    LLMResponse *resp;
    char *line;
    size_t line_len;
    size_t line_cap;
    void (*on_chunk)(const char *, void *);
    void *userdata;
} StreamParser;

static int stream_parser_feed(StreamParser *p, const char *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (p->line_len + 1 >= p->line_cap)
        {
            size_t new_cap = p->line_cap ? p->line_cap * 2 : 256;
            char *new_line = realloc(p->line, new_cap);
            if (!new_line) return -1;
            p->line = new_line;
            p->line_cap = new_cap;
        }
        p->line[p->line_len++] = bytes[i];
        if (bytes[i] != '\n') continue;

        /* complete line: strip trailing \r, then parse "data: " events */
        if (p->line_len > 1 && p->line[p->line_len - 2] == '\r')
            p->line[p->line_len - 2] = '\0';
        else
            p->line[p->line_len - 1] = '\0';
        p->line_len = 0;

        if (strncmp(p->line, "data: ", 6) == 0 &&
            strcmp(p->line + 6, "[DONE]") != 0)
        {
            if (parse_stream_event(p->resp, p->line + 6,
                                   p->on_chunk, p->userdata) != 0)
                return -1;
        }
    }
    return 0;
}

static int stream_parser_finish(StreamParser *p)
{
    if (p->line_len == 0) return 0;
    /* final partial line without a trailing newline */
    if (p->line_len > 0 && p->line[p->line_len - 1] == '\r')
        p->line[--p->line_len] = '\0';
    else
        p->line[p->line_len] = '\0';
    p->line_len = 0;
    if (strncmp(p->line, "data: ", 6) == 0 &&
        strcmp(p->line + 6, "[DONE]") != 0)
        return parse_stream_event(p->resp, p->line + 6,
                                  p->on_chunk, p->userdata);
    return 0;
}

#ifdef OPENAI_COMPATIBLE_TEST
LLMResponse *openai_compatible_test_parse_stream(
    const char *input, void (*on_chunk)(const char *, void *), void *userdata)
{
    if (!input) return NULL;
    LLMResponse *resp = llm_response_create();
    if (!resp) return NULL;
    StreamParser p = {0};
    p.resp = resp;
    p.on_chunk = on_chunk;
    p.userdata = userdata;
    if (stream_parser_feed(&p, input, strlen(input)) != 0 ||
        stream_parser_finish(&p) != 0)
    {
        free(p.line);
        llm_response_free(resp);
        return NULL;
    }
    free(p.line);
    if (!resp->content)
    {
        resp->content = str_dup("");
        if (!resp->content) { llm_response_free(resp); return NULL; }
    }
    return resp;
}

LLMResponse *openai_compatible_test_stream_fragments(
    const char **fragments, size_t *lengths, int count,
    void (*on_chunk)(const char *, void *), void *userdata)
{
    if (!fragments || count <= 0) return NULL;
    LLMResponse *resp = llm_response_create();
    if (!resp) return NULL;
    StreamParser p = {0};
    p.resp = resp;
    p.on_chunk = on_chunk;
    p.userdata = userdata;
    for (int i = 0; i < count; i++)
    {
        size_t len = lengths ? lengths[i] : strlen(fragments[i]);
        if (stream_parser_feed(&p, fragments[i], len) != 0)
        {
            free(p.line);
            llm_response_free(resp);
            return NULL;
        }
    }
    if (stream_parser_finish(&p) != 0)
    {
        free(p.line);
        llm_response_free(resp);
        return NULL;
    }
    free(p.line);
    if (!resp->content)
    {
        resp->content = str_dup("");
        if (!resp->content) { llm_response_free(resp); return NULL; }
    }
    return resp;
}
#endif

static char *build_url(const char *base_url)
{
    /* Accept any base_url form:
     *   https://host/v1/chat/completions  -> used as-is (OpenCode Zen's
     *                                         documented full endpoint)
     *   https://host/v1                   -> append /chat/completions,
     *                                         so https://opencode.ai/zen/v1
     *                                         does not become .../v1/v1/...
     *   https://host                      -> append /v1/chat/completions
     */
    static const char chat_path[] = "/chat/completions";
    size_t base_len = strlen(base_url);
    if (base_len >= sizeof(chat_path) - 1 &&
        strcmp(base_url + base_len - (sizeof(chat_path) - 1), chat_path) == 0)
        return str_dup(base_url);

    static const char v1_path[] = "/v1";
    if (base_len >= sizeof(v1_path) - 1 &&
        strcmp(base_url + base_len - (sizeof(v1_path) - 1), v1_path) == 0)
    {
        char *url = NULL;
        if (asprintf(&url, "%s/chat/completions", base_url) < 0) return NULL;
        return url;
    }

    char *url = NULL;
    if (asprintf(&url, "%s/v1/chat/completions", base_url) < 0) return NULL;
    return url;
}

#ifdef OPENAI_COMPATIBLE_TEST
char *openai_compatible_test_build_url(const char *base_url)
{
    return build_url(base_url);
}
#endif

static char *build_messages_json(Message *messages, int count, const char *system_prompt,
                                  const char *model)
{
    (void)model;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    if (system_prompt && system_prompt[0])
    {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", system_prompt);
        cJSON_AddItemToArray(arr, sys);
    }

    for (int i = 0; i < count; i++)
    {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", messages[i].role ? messages[i].role : "user");
        cJSON_AddStringToObject(msg, "content", messages[i].content ? messages[i].content : "");

        if (messages[i].tool_calls_count > 0 && messages[i].tool_calls)
        {
            cJSON *tc_arr = cJSON_CreateArray();
            for (int j = 0; j < messages[i].tool_calls_count; j++)
            {
                cJSON *tc = cJSON_CreateObject();
                cJSON_AddStringToObject(tc, "id", messages[i].tool_calls[j].id ? messages[i].tool_calls[j].id : "");
                cJSON_AddStringToObject(tc, "type", "function");
                cJSON *fn = cJSON_CreateObject();
                cJSON_AddStringToObject(fn, "name", messages[i].tool_calls[j].name ? messages[i].tool_calls[j].name : "");
                cJSON_AddStringToObject(fn, "arguments", messages[i].tool_calls[j].arguments ? messages[i].tool_calls[j].arguments : "{}");
                cJSON_AddItemToObject(tc, "function", fn);
                cJSON_AddItemToArray(tc_arr, tc);
            }
            cJSON_AddItemToObject(msg, "tool_calls", tc_arr);
        }

        if (strcmp(messages[i].role, "tool") == 0)
        {
            cJSON_AddStringToObject(msg, "tool_call_id",
                messages[i].tool_call_id ? messages[i].tool_call_id : "");
        }

        cJSON_AddItemToArray(arr, msg);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

static LLMResponse *openai_compatible_parse_response(const char *raw)
{
    LLMResponse *resp = llm_response_create();
    if (!resp) return NULL;

    cJSON *json = cJSON_Parse(raw);
    if (!json)
    {
        log_error("openai compatible parse failed", "response", raw, NULL);
        free(resp);
        return NULL;
    }

    cJSON *choices = cJSON_GetObjectItem(json, "choices");
    if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0)
    {
        cJSON_Delete(json);
        free(resp);
        return NULL;
    }

    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    if (!message)
    {
        cJSON *delta = cJSON_GetObjectItem(choice, "delta");
        if (delta) message = delta;
    }

    if (!message) { cJSON_Delete(json); free(resp); return NULL; }

    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsString(content))
        resp->content = str_dup(cJSON_GetStringValue(content));

    cJSON *tc_arr = cJSON_GetObjectItem(message, "tool_calls");
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

                    cJSON *id = cJSON_GetObjectItem(tc, "id");
                    cJSON *name = cJSON_GetObjectItem(fn, "name");
                    cJSON *args = cJSON_GetObjectItem(fn, "arguments");

                    resp->tool_calls[i].id = str_dup(id && cJSON_IsString(id)
                        ? cJSON_GetStringValue(id) : "");
                    resp->tool_calls[i].name = str_dup(name && cJSON_IsString(name)
                        ? cJSON_GetStringValue(name) : "");

                    if (args && cJSON_IsString(args))
                    {
                        resp->tool_calls[i].arguments = str_dup(cJSON_GetStringValue(args));
                    }
                    else if (args)
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

/* Common request setup shared by the buffered and live-streaming paths:
 * URL resolution, JSON headers, optional Bearer auth. Returns 0 and fills
 * the out-params on success; -1 on failure (caller owns cleanup). */
static int chat_request_setup(CURL *curl, const char *base_url,
                              const char *api_token, const char *json_body,
                              int timeout,
                              struct curl_slist **headers_out,
                              char **url_out)
{
    char *url = build_url(base_url);
    if (!url) return -1;

    struct curl_slist *headers =
        curl_slist_append(NULL, "Content-Type: application/json");
    if (!headers) { free(url); return -1; }

    if (api_token && api_token[0])
    {
        /* Bearer auth is required by real OpenAI and supported by most
         * OpenAI-compatible servers (LM Studio, vLLM). */
        char *auth = NULL;
        if (asprintf(&auth, "Authorization: Bearer %s", api_token) < 0)
        {
            curl_slist_free_all(headers);
            free(url);
            return -1;
        }
        headers = curl_slist_append(headers, auth);
        free(auth);
        if (!headers) { free(url); return -1; }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);

    *headers_out = headers;
    *url_out = url;
    return 0;
}

static char *openai_compatible_chat_request(const char *base_url,
                                            const char *api_token,
                                            const char *json_body,
                                            int timeout)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    char *url = NULL;
    struct curl_slist *headers = NULL;
    if (chat_request_setup(curl, base_url, api_token, json_body, timeout,
                           &headers, &url) != 0)
    {
        curl_easy_cleanup(curl);
        return NULL;
    }

    WriteBuf buf = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    free(url);

    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        log_error("openai compatible request failed", "error", curl_easy_strerror(res), NULL);
        free(buf.data);
        return NULL;
    }

    /* curl_easy_perform returns CURLE_OK even on HTTP 4xx/5xx — check the
     * status explicitly, or error bodies (e.g. a 404 from the wrong
     * endpoint) get parsed as a success and the caller reports an empty
     * reply instead of an error. */
    if (http_status < 200 || http_status >= 300)
    {
        char status_str[16];
        snprintf(status_str, sizeof(status_str), "%ld", http_status);
        log_error("openai compatible request returned non-2xx", "status",
                  status_str, "body", buf.data ? buf.data : "", NULL);
        free(buf.data);
        return NULL;
    }

    return buf.data;
}

typedef struct {
    CURL *curl;
    WriteBuf raw;         /* error-body capture only */
    StreamParser parser;
    int status_checked;
    int failed;           /* non-2xx: buffer raw, never emit chunks */
} LiveCtx;

static size_t live_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    LiveCtx *ctx = (LiveCtx *)userdata;
    size_t total = size * nmemb;

    if (!ctx->status_checked)
    {
        ctx->status_checked = 1;
        long status = 0;
        if (ctx->curl)
            curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &status);
        ctx->failed = (status < 200 || status >= 300);
    }

    if (ctx->failed)
    {
        /* Error body: keep raw bytes for the error log only. */
        if (write_cb(ptr, size, nmemb, &ctx->raw) == 0) return 0;
        return total;
    }

    /* Emit chunks to the caller as complete SSE lines arrive, so the
     * frontend renders tokens while the response is still in flight. */
    if (stream_parser_feed(&ctx->parser, ptr, total) != 0) return 0;
    return total;
}

/* Live streaming variant of openai_compatible_chat_request: parses SSE
 * inside the curl write callback and returns the fully aggregated
 * LLMResponse (content + tool calls). on_chunk fires per content delta
 * as bytes arrive. */
static LLMResponse *openai_compatible_chat_stream_request(
    const char *base_url, const char *api_token, const char *json_body,
    int timeout, void (*on_chunk)(const char *, void *), void *userdata)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    char *url = NULL;
    struct curl_slist *headers = NULL;
    if (chat_request_setup(curl, base_url, api_token, json_body, timeout,
                           &headers, &url) != 0)
    {
        curl_easy_cleanup(curl);
        return NULL;
    }

    LLMResponse *resp = llm_response_create();
    if (!resp)
    {
        curl_slist_free_all(headers);
        free(url);
        curl_easy_cleanup(curl);
        return NULL;
    }

    LiveCtx ctx = {0};
    ctx.curl = curl;
    ctx.parser.resp = resp;
    ctx.parser.on_chunk = on_chunk;
    ctx.parser.userdata = userdata;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, live_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    free(url);

    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        log_error("openai compatible request failed", "error",
                  curl_easy_strerror(res), NULL);
        free(ctx.raw.data);
        free(ctx.parser.line);
        llm_response_free(resp);
        return NULL;
    }

    if (http_status < 200 || http_status >= 300)
    {
        char status_str[16];
        snprintf(status_str, sizeof(status_str), "%ld", http_status);
        log_error("openai compatible request returned non-2xx", "status",
                  status_str, "body",
                  ctx.raw.data ? ctx.raw.data : "", NULL);
        free(ctx.raw.data);
        free(ctx.parser.line);
        llm_response_free(resp);
        return NULL;
    }

    if (stream_parser_finish(&ctx.parser) != 0)
    {
        free(ctx.raw.data);
        free(ctx.parser.line);
        llm_response_free(resp);
        return NULL;
    }
    free(ctx.raw.data);
    free(ctx.parser.line);

    if (!resp->content)
    {
        resp->content = str_dup("");
        if (!resp->content) { llm_response_free(resp); return NULL; }
    }
    return resp;
}

int openai_compatible_reasoning_effort_valid(const char *effort)
{
    if (!effort || !effort[0]) return 1;
    return strcmp(effort, "low") == 0 ||
           strcmp(effort, "medium") == 0 ||
           strcmp(effort, "high") == 0 ||
           strcmp(effort, "max") == 0 ||
           strcmp(effort, "none") == 0;
}

/* Renders the "reasoning_effort" JSON field (with trailing comma) into buf,
 * or an empty string when no effort is set. Returns -1 on invalid effort or
 * a buffer too small for the value. */
static int reasoning_effort_fragment(const char *effort, char *buf, size_t cap)
{
    if (!effort || !effort[0])
    {
        buf[0] = '\0';
        return 0;
    }
    if (!openai_compatible_reasoning_effort_valid(effort)) return -1;
    if (snprintf(buf, cap, "\"reasoning_effort\":\"%s\",", effort) >= (int)cap)
        return -1;
    return 0;
}

/* Builds a chat-completions request body. tools_json and json_schema are
 * mutually exclusive; force_json_format forces the response_format json
 * variant used by extract_structured. Returns a caller-owned string, or
 * NULL when effort is invalid or allocation fails. */
static char *build_body(const char *model, const char *msgs_json, int stream,
                        double temperature, const char *tools_json,
                        const char *json_schema, int force_json_format,
                        const char *effort)
{
    char effort_frag[64];
    if (reasoning_effort_fragment(effort, effort_frag,
                                  sizeof(effort_frag)) != 0)
        return NULL;

    const char *stream_str = stream ? "true" : "false";
    char *body = NULL;
    if (tools_json && tools_json[0])
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":%s,"
                     "%s\"temperature\":%.2f,\"tools\":%s}",
                     model, msgs_json, stream_str, effort_frag,
                     temperature, tools_json) < 0)
            return NULL;
    }
    else if (json_schema && json_schema[0])
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":false,"
                     "%s\"temperature\":%.2f,"
                     "\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{\"schema\":%s,\"strict\":true}}}",
                     model, msgs_json, effort_frag, temperature,
                     json_schema) < 0)
            return NULL;
    }
    else if (force_json_format)
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":false,"
                     "%s\"temperature\":%.2f,"
                     "\"response_format\":{\"type\":\"json_object\"}}",
                     model, msgs_json, effort_frag, temperature) < 0)
            return NULL;
    }
    else
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":%s,"
                     "%s\"temperature\":%.2f}",
                     model, msgs_json, stream_str, effort_frag,
                     temperature) < 0)
            return NULL;
    }
    return body;
}

static LLMResponse *openai_compatible_chat(LLMProvider *self, Message *messages, int count,
                                           const char *model, double temperature, int timeout,
                                           const char *tools_json)
{
    OpenAICompatCtx *ctx = self->ctx;

    char *msgs_json = build_messages_json(messages, count, NULL, model);
    if (!msgs_json) return NULL;

    char *body = build_body(model, msgs_json, 0, temperature, tools_json,
                            NULL, 0, ctx->effort);
    free(msgs_json);
    if (!body) return NULL;

    log_debug("openai compatible request", "model", model, NULL);

    char *raw = openai_compatible_chat_request(ctx->base_url, ctx->api_token,
                                               body, timeout);
    free(body);

    if (!raw) return NULL;

    LLMResponse *resp = openai_compatible_parse_response(raw);
    free(raw);
    return resp;
}

static LLMResponse *openai_compatible_chat_streaming(LLMProvider *self, Message *messages, int count,
                                                     const char *model, double temperature, int timeout,
                                                     void (*on_chunk)(const char *, void *),
                                                     void *userdata,
                                                     const char *tools_json)
{
    OpenAICompatCtx *ctx = self->ctx;

    char *msgs_json = build_messages_json(messages, count, NULL, model);
    if (!msgs_json) return NULL;

    char *body = build_body(model, msgs_json, 1, temperature, tools_json,
                            NULL, 0, ctx->effort);
    free(msgs_json);
    if (!body) return NULL;

    /* Live streaming: chunks fire via on_chunk as SSE lines arrive, and
     * the aggregated response is returned once the stream completes. */
    LLMResponse *resp = openai_compatible_chat_stream_request(
        ctx->base_url, ctx->api_token, body, timeout, on_chunk, userdata);
    free(body);
    return resp;
}

static LLMResponse *openai_compatible_extract_structured(LLMProvider *self, Message *messages, int count,
                                                         const char *model, double temperature, int timeout,
                                                         const char *json_schema)
{
    OpenAICompatCtx *ctx = self->ctx;

    char *msgs_json = build_messages_json(messages, count, NULL, model);
    if (!msgs_json) return NULL;

    char *body = build_body(model, msgs_json, 0, temperature, NULL,
                            json_schema, 1, ctx->effort);
    free(msgs_json);
    if (!body) return NULL;

    char *raw = openai_compatible_chat_request(ctx->base_url, ctx->api_token,
                                               body, timeout);
    free(body);

    if (!raw) return NULL;

    LLMResponse *resp = openai_compatible_parse_response(raw);
    free(raw);
    return resp;
}

static void openai_compatible_destroy(LLMProvider *self)
{
    if (!self) return;
    OpenAICompatCtx *ctx = self->ctx;
    free(ctx->base_url);
    free(ctx->api_token);
    free(ctx->effort);
    free(ctx);
    free(self);
}

LLMProvider *openai_compatible_provider_create(const char *base_url,
                                               const char *api_token,
                                               const char *effort)
{
    if (effort && !openai_compatible_reasoning_effort_valid(effort))
    {
        log_error("OpenAI-compatible provider rejected invalid reasoning effort",
                  "effort", effort, NULL);
        return NULL;
    }

    LLMProvider *p = calloc(1, sizeof(LLMProvider));
    if (!p) return NULL;

    OpenAICompatCtx *ctx = calloc(1, sizeof(OpenAICompatCtx));
    if (!ctx) { free(p); return NULL; }

    ctx->base_url = str_dup(base_url ? base_url : "https://api.openai.com");
    if (!ctx->base_url) { free(ctx); free(p); return NULL; }

    if (api_token && api_token[0])
    {
        ctx->api_token = str_dup(api_token);
        if (!ctx->api_token) { free(ctx->base_url); free(ctx); free(p); return NULL; }
    }

    if (effort && effort[0])
    {
        ctx->effort = str_dup(effort);
        if (!ctx->effort) { free(ctx->base_url); free(ctx->api_token); free(ctx); free(p); return NULL; }
    }

    p->chat = openai_compatible_chat;
    p->chat_streaming = openai_compatible_chat_streaming;
    p->extract_structured = openai_compatible_extract_structured;
    p->destroy = openai_compatible_destroy;
    p->ctx = ctx;
    return p;
}

#ifdef OPENAI_COMPATIBLE_TEST
/* Test hook: builds a request body exactly as the provider does, with the
 * given effort. Returns a caller-owned string, or NULL on invalid effort. */
char *openai_compatible_test_build_body(const char *model, const char *msgs_json,
                                        int stream, double temperature,
                                        const char *tools_json,
                                        const char *json_schema,
                                        int force_json_format,
                                        const char *effort)
{
    return build_body(model, msgs_json, stream, temperature, tools_json,
                      json_schema, force_json_format, effort);
}
#endif
