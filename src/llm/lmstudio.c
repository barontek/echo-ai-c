#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "provider.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

typedef struct {
    char *base_url;
} LMStudioCtx;

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

static int parse_stream_response(char *raw, LLMResponse *resp,
                                 void (*on_chunk)(const char *, void *),
                                 void *userdata)
{
    if (!raw) return -1;
    char *save = NULL;
    char *line = strtok_r(raw, "\n", &save);
    while (line)
    {
        size_t line_len = strlen(line);
        if (line_len > 0 && line[line_len - 1] == '\r') line[--line_len] = '\0';
        if (strncmp(line, "data: ", 6) == 0)
        {
            const char *data = line + 6;
            if (strcmp(data, "[DONE]") != 0 &&
                parse_stream_event(resp, data, on_chunk, userdata) != 0)
                return -1;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    if (!resp->content)
    {
        resp->content = str_dup("");
        if (!resp->content) return -1;
    }
    return 0;
}

#ifdef LMSTUDIO_TEST
LLMResponse *lmstudio_test_parse_stream(
    const char *input, void (*on_chunk)(const char *, void *), void *userdata)
{
    if (!input) return NULL;
    char *raw = str_dup(input);
    LLMResponse *resp = llm_response_create();
    if (!raw || !resp)
    {
        free(raw);
        llm_response_free(resp);
        return NULL;
    }
    if (parse_stream_response(raw, resp, on_chunk, userdata) != 0)
    {
        free(raw);
        llm_response_free(resp);
        return NULL;
    }
    free(raw);
    return resp;
}
#endif

static char *build_url(const char *base_url)
{
    char *url = NULL;
    if (asprintf(&url, "%s/v1/chat/completions", base_url) < 0) return NULL;
    return url;
}

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

static LLMResponse *lmstudio_parse_response(const char *raw)
{
    LLMResponse *resp = llm_response_create();
    if (!resp) return NULL;

    cJSON *json = cJSON_Parse(raw);
    if (!json)
    {
        log_error("lmstudio parse failed", "response", raw, NULL);
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

static char *lmstudio_chat_request(const char *base_url, const char *json_body,
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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    free(url);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        log_error("lmstudio request failed", "error", curl_easy_strerror(res), NULL);
        free(buf.data);
        return NULL;
    }

    (void)stream;
    (void)on_chunk;
    (void)userdata;

    return buf.data;
}

static LLMResponse *lmstudio_chat(LLMProvider *self, Message *messages, int count,
                                   const char *model, double temperature, int timeout,
                                   const char *tools_json)
{
    LMStudioCtx *ctx = self->ctx;

    char *msgs_json = build_messages_json(messages, count, NULL, model);
    if (!msgs_json) return NULL;

    char *body = NULL;
    if (tools_json && tools_json[0])
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":false,"
                     "\"temperature\":%.2f,\"tools\":%s}",
                     model, msgs_json, temperature, tools_json) < 0)
        {
            free(msgs_json);
            return NULL;
        }
    }
    else
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":false,"
                     "\"temperature\":%.2f}",
                     model, msgs_json, temperature) < 0)
        {
            free(msgs_json);
            return NULL;
        }
    }
    free(msgs_json);

    log_debug("lmstudio request", "model", model, NULL);

    char *raw = lmstudio_chat_request(ctx->base_url, body, timeout, 0, NULL, NULL);
    free(body);

    if (!raw) return NULL;

    LLMResponse *resp = lmstudio_parse_response(raw);
    free(raw);
    return resp;
}

static LLMResponse *lmstudio_chat_streaming(LLMProvider *self, Message *messages, int count,
                                             const char *model, double temperature, int timeout,
                                             void (*on_chunk)(const char *, void *),
                                             void *userdata,
                                             const char *tools_json)
{
    LMStudioCtx *ctx = self->ctx;

    char *msgs_json = build_messages_json(messages, count, NULL, model);
    if (!msgs_json) return NULL;

    char *body = NULL;
    if (tools_json && tools_json[0])
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":true,"
                     "\"temperature\":%.2f,\"tools\":%s}",
                     model, msgs_json, temperature, tools_json) < 0)
        {
            free(msgs_json);
            return NULL;
        }
    }
    else
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":true,"
                     "\"temperature\":%.2f}",
                     model, msgs_json, temperature) < 0)
        {
            free(msgs_json);
            return NULL;
        }
    }
    free(msgs_json);

    LLMResponse *resp = llm_response_create();
    if (!resp) { free(body); return NULL; }

    char *raw = lmstudio_chat_request(ctx->base_url, body, timeout, 1,
                                       on_chunk, userdata);
    free(body);

    if (!raw)
    {
        llm_response_free(resp);
        return NULL;
    }

    if (parse_stream_response(raw, resp, on_chunk, userdata) != 0)
    {
        free(raw);
        llm_response_free(resp);
        return NULL;
    }
    free(raw);
    return resp;
}

static LLMResponse *lmstudio_extract_structured(LLMProvider *self, Message *messages, int count,
                                                 const char *model, double temperature, int timeout,
                                                 const char *json_schema)
{
    LMStudioCtx *ctx = self->ctx;

    char *msgs_json = build_messages_json(messages, count, NULL, model);
    if (!msgs_json) return NULL;

    char *body = NULL;
    if (json_schema && json_schema[0])
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":false,"
                     "\"temperature\":%.2f,"
                     "\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{\"schema\":%s,\"strict\":true}}}",
                     model, msgs_json, temperature, json_schema) < 0)
        { free(msgs_json); return NULL; }
    }
    else
    {
        if (asprintf(&body, "{\"model\":\"%s\",\"messages\":%s,\"stream\":false,"
                     "\"temperature\":%.2f,"
                     "\"response_format\":{\"type\":\"json_object\"}}",
                     model, msgs_json, temperature) < 0)
        { free(msgs_json); return NULL; }
    }
    free(msgs_json);

    char *raw = lmstudio_chat_request(ctx->base_url, body, timeout, 0, NULL, NULL);
    free(body);

    if (!raw) return NULL;

    LLMResponse *resp = lmstudio_parse_response(raw);
    free(raw);
    return resp;
}

static void lmstudio_destroy(LLMProvider *self)
{
    if (!self) return;
    LMStudioCtx *ctx = self->ctx;
    free(ctx->base_url);
    free(ctx);
    free(self);
}

LLMProvider *lmstudio_provider_create(const char *base_url)
{
    LLMProvider *p = calloc(1, sizeof(LLMProvider));
    if (!p) return NULL;

    LMStudioCtx *ctx = calloc(1, sizeof(LMStudioCtx));
    if (!ctx) { free(p); return NULL; }

    ctx->base_url = str_dup(base_url ? base_url : "http://localhost:1234");

    p->chat = lmstudio_chat;
    p->chat_streaming = lmstudio_chat_streaming;
    p->extract_structured = lmstudio_extract_structured;
    p->destroy = lmstudio_destroy;
    p->ctx = ctx;
    return p;
}
