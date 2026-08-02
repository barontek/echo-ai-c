#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "openai.h"
#include "openai_oauth.h"
#include "../utils/string_utils.h"

#define CODEX_ENDPOINT "https://chatgpt.com/backend-api/codex/responses"

typedef struct { OpenAIOAuth *auth; } OpenAICtx;
typedef struct { char *data; size_t len; size_t cap; } ResponseBuffer;
typedef struct {
    LLMResponse *response;
    char *line;
    size_t line_len;
    size_t line_cap;
    void (*on_chunk)(const char *, void *);
    void *userdata;
} StreamContext;

static void clear_secret(char **value)
{
    if (!value || !*value) return;
    memset(*value, 0, strlen(*value));
    free(*value);
    *value = NULL;
}

static size_t response_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    ResponseBuffer *buffer = userdata;
    size_t total = size * nmemb;
    if (total > SIZE_MAX - buffer->len - 1) return 0;
    size_t needed = buffer->len + total + 1;
    if (needed > buffer->cap)
    {
        size_t cap = buffer->cap ? buffer->cap * 2 : 1024;
        while (cap < needed)
        {
            if (cap > SIZE_MAX / 2) return 0;
            cap *= 2;
        }
        char *grown = realloc(buffer->data, cap);
        if (!grown) return 0;
        buffer->data = grown;
        buffer->cap = cap;
    }
    memcpy(buffer->data + buffer->len, ptr, total);
    buffer->len += total;
    buffer->data[buffer->len] = '\0';
    return total;
}

static int append_text(char **target, const char *value)
{
    if (!value) return 0;
    size_t old_len = *target ? strlen(*target) : 0;
    size_t add_len = strlen(value);
    if (add_len > SIZE_MAX - old_len - 1) return -1;
    char *grown = realloc(*target, old_len + add_len + 1);
    if (!grown) return -1;
    memcpy(grown + old_len, value, add_len + 1);
    *target = grown;
    return 0;
}

static int ensure_tool_call(LLMResponse *response, int index)
{
    if (index < 0) return -1;
    if (index < response->tool_calls_count) return 0;
    size_t count = (size_t)index + 1;
    if (count > SIZE_MAX / sizeof(ToolCall)) return -1;
    ToolCall *grown = realloc(response->tool_calls, count * sizeof(ToolCall));
    if (!grown) return -1;
    memset(grown + response->tool_calls_count, 0,
           (count - (size_t)response->tool_calls_count) * sizeof(ToolCall));
    response->tool_calls = grown;
    response->tool_calls_count = index + 1;
    return 0;
}

static cJSON *responses_tools(const char *tools_json)
{
    cJSON *result = cJSON_CreateArray();
    if (!result) return NULL;
    if (!tools_json || !tools_json[0]) return result;
    cJSON *source = cJSON_Parse(tools_json);
    if (!source || !cJSON_IsArray(source))
    {
        cJSON_Delete(source);
        return result;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, source)
    {
        cJSON *function = cJSON_GetObjectItemCaseSensitive(item, "function");
        cJSON *name = function ? cJSON_GetObjectItemCaseSensitive(function, "name") : NULL;
        if (!function || !cJSON_IsObject(function) || !name || !cJSON_IsString(name)) continue;
        cJSON *converted = cJSON_CreateObject();
        if (!converted) { cJSON_Delete(source); cJSON_Delete(result); return NULL; }
        cJSON_AddStringToObject(converted, "type", "function");
        cJSON_AddStringToObject(converted, "name", name->valuestring);
        cJSON *description = cJSON_GetObjectItemCaseSensitive(function, "description");
        cJSON *parameters = cJSON_GetObjectItemCaseSensitive(function, "parameters");
        if (description && cJSON_IsString(description))
            cJSON_AddStringToObject(converted, "description", description->valuestring);
        if (parameters)
            cJSON_AddItemToObject(converted, "parameters", cJSON_Duplicate(parameters, 1));
        cJSON_AddItemToArray(result, converted);
    }
    cJSON_Delete(source);
    return result;
}

static int add_input_messages(cJSON *input, Message *messages, int count, cJSON *root)
{
    for (int i = 0; i < count; i++)
    {
        Message *message = &messages[i];
        if (!message->role) continue;
        if (strcmp(message->role, "system") == 0)
        {
            if (message->content && !cJSON_AddStringToObject(root, "instructions", message->content))
                return -1;
            continue;
        }
        if (strcmp(message->role, "tool") == 0)
        {
            cJSON *output = cJSON_CreateObject();
            if (!output) return -1;
            cJSON_AddStringToObject(output, "type", "function_call_output");
            cJSON_AddStringToObject(output, "call_id", message->tool_call_id ? message->tool_call_id : "");
            cJSON_AddStringToObject(output, "output", message->content ? message->content : "");
            cJSON_AddItemToArray(input, output);
            continue;
        }
        cJSON *entry = cJSON_CreateObject();
        if (!entry) return -1;
        cJSON_AddStringToObject(entry, "role", message->role);
        cJSON_AddStringToObject(entry, "content", message->content ? message->content : "");
        cJSON_AddItemToArray(input, entry);
        for (int j = 0; j < message->tool_calls_count; j++)
        {
            ToolCall *call = &message->tool_calls[j];
            cJSON *function = cJSON_CreateObject();
            if (!function) return -1;
            cJSON_AddStringToObject(function, "type", "function_call");
            cJSON_AddStringToObject(function, "call_id", call->id ? call->id : "");
            cJSON_AddStringToObject(function, "name", call->name ? call->name : "");
            cJSON_AddStringToObject(function, "arguments", call->arguments ? call->arguments : "{}");
            cJSON_AddItemToArray(input, function);
        }
    }
    return 0;
}

static char *build_request_body(Message *messages, int count, const char *model,
                                int stream, const char *tools_json)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *input = cJSON_CreateArray();
    if (!root || !input) { cJSON_Delete(root); cJSON_Delete(input); return NULL; }
    cJSON_AddStringToObject(root, "model", model ? model : "");
    cJSON_AddBoolToObject(root, "stream", stream);
    cJSON_AddBoolToObject(root, "store", 0);
    if (add_input_messages(input, messages, count, root) != 0)
    { cJSON_Delete(root); cJSON_Delete(input); return NULL; }
    cJSON_AddItemToObject(root, "input", input);
    cJSON *tools = responses_tools(tools_json);
    if (!tools) { cJSON_Delete(root); return NULL; }
    if (cJSON_GetArraySize(tools) > 0) cJSON_AddItemToObject(root, "tools", tools);
    else cJSON_Delete(tools);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static int request_setup(CURL *curl, const char *body, OpenAIOAuth *auth,
                         struct curl_slist **headers_out)
{
    char *token = NULL;
    char *account = NULL;
    if (openai_oauth_get_access_token(auth, &token, &account) != 0) return -1;
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    char *authorization = NULL;
    if (headers && asprintf(&authorization, "Authorization: Bearer %s", token) >= 0)
        headers = curl_slist_append(headers, authorization);
    if (headers) headers = curl_slist_append(headers, "originator: echo-ai");
    if (headers) headers = curl_slist_append(headers, "User-Agent: echo-ai");
    if (headers && account)
    {
        char *account_header = NULL;
        if (asprintf(&account_header, "ChatGPT-Account-Id: %s", account) >= 0)
            headers = curl_slist_append(headers, account_header);
        free(account_header);
    }
    int result = -1;
    if (headers && curl_easy_setopt(curl, CURLOPT_URL, CODEX_ENDPOINT) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_POST, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L) == CURLE_OK)
    { *headers_out = headers; result = 0; }
    else curl_slist_free_all(headers);
    free(authorization);
    clear_secret(&token);
    free(account);
    return result;
}

static LLMResponse *parse_response(const char *raw)
{
    cJSON *root = cJSON_Parse(raw);
    if (!root) return NULL;
    LLMResponse *response = llm_response_create();
    if (!response) { cJSON_Delete(root); return NULL; }
    cJSON *output = cJSON_GetObjectItemCaseSensitive(root, "output");
    cJSON *item = NULL;
    if (output && cJSON_IsArray(output)) cJSON_ArrayForEach(item, output)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!type || !cJSON_IsString(type)) continue;
        if (strcmp(type->valuestring, "message") == 0)
        {
            cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
            cJSON *part = content && cJSON_IsArray(content) ? cJSON_GetArrayItem(content, 0) : NULL;
            cJSON *text = part ? cJSON_GetObjectItemCaseSensitive(part, "text") : NULL;
            if (text && cJSON_IsString(text) && append_text(&response->content, text->valuestring) != 0)
            { llm_response_free(response); cJSON_Delete(root); return NULL; }
        }
        else if (strcmp(type->valuestring, "function_call") == 0)
        {
            int index = response->tool_calls_count;
            if (ensure_tool_call(response, index) != 0)
            { llm_response_free(response); cJSON_Delete(root); return NULL; }
            ToolCall *call = &response->tool_calls[index];
            cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "call_id");
            cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
            cJSON *args = cJSON_GetObjectItemCaseSensitive(item, "arguments");
            call->id = id && cJSON_IsString(id) ? str_dup(id->valuestring) : str_dup("");
            call->name = name && cJSON_IsString(name) ? str_dup(name->valuestring) : str_dup("");
            call->arguments = args && cJSON_IsString(args) ? str_dup(args->valuestring) : str_dup("{}");
            if (!call->id || !call->name || !call->arguments)
            { llm_response_free(response); cJSON_Delete(root); return NULL; }
        }
    }
    if (!response->content) response->content = str_dup("");
    cJSON_Delete(root);
    if (!response->content) { llm_response_free(response); return NULL; }
    return response;
}

static int parse_stream_event(StreamContext *context, const char *text)
{
    cJSON *json = cJSON_Parse(text);
    if (!json) return -1;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    const char *value = type && cJSON_IsString(type) ? type->valuestring : "";
    if (strcmp(value, "response.output_text.delta") == 0)
    {
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(json, "delta");
        if (delta && cJSON_IsString(delta))
        {
            if (append_text(&context->response->content, delta->valuestring) != 0)
            { cJSON_Delete(json); return -1; }
            if (context->on_chunk) context->on_chunk(delta->valuestring, context->userdata);
        }
    }
    else if (strcmp(value, "response.function_call_arguments.delta") == 0)
    {
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(json, "delta");
        if (delta && cJSON_IsString(delta) && ensure_tool_call(context->response, 0) == 0 &&
            append_text(&context->response->tool_calls[0].arguments, delta->valuestring) != 0)
        { cJSON_Delete(json); return -1; }
    }
    else if (strcmp(value, "response.output_item.done") == 0)
    {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(json, "item");
        cJSON *item_type = item ? cJSON_GetObjectItemCaseSensitive(item, "type") : NULL;
        if (item && item_type && cJSON_IsString(item_type) && strcmp(item_type->valuestring, "function_call") == 0)
        {
            if (ensure_tool_call(context->response, 0) != 0)
            { cJSON_Delete(json); return -1; }
            cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "call_id");
            cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
            if (id && cJSON_IsString(id)) { free(context->response->tool_calls[0].id); context->response->tool_calls[0].id = str_dup(id->valuestring); }
            if (name && cJSON_IsString(name)) { free(context->response->tool_calls[0].name); context->response->tool_calls[0].name = str_dup(name->valuestring); }
        }
    }
    cJSON_Delete(json);
    return 0;
}

static size_t stream_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    StreamContext *context = userdata;
    size_t total = size * nmemb;
    const char *bytes = ptr;
    for (size_t i = 0; i < total; i++)
    {
        if (context->line_len + 1 >= context->line_cap)
        {
            size_t cap = context->line_cap ? context->line_cap * 2 : 512;
            char *grown = realloc(context->line, cap);
            if (!grown) return 0;
            context->line = grown;
            context->line_cap = cap;
        }
        context->line[context->line_len++] = bytes[i];
        if (bytes[i] != '\n') continue;
        if (context->line_len > 1 && context->line[context->line_len - 2] == '\r')
            context->line[context->line_len - 2] = '\0';
        else context->line[context->line_len - 1] = '\0';
        if (strncmp(context->line, "data: ", 6) == 0 && strcmp(context->line + 6, "[DONE]") != 0 &&
            parse_stream_event(context, context->line + 6) != 0) return 0;
        context->line_len = 0;
    }
    return total;
}

static LLMResponse *openai_chat(LLMProvider *provider, Message *messages, int count,
                                const char *model, double temperature, int timeout,
                                const char *tools_json)
{
    (void)temperature; (void)timeout;
    OpenAICtx *context = provider->ctx;
    char *body = build_request_body(messages, count, model, 0, tools_json);
    if (!body) return NULL;
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    ResponseBuffer buffer = {0};
    LLMResponse *response = NULL;
    CURLcode performed = CURLE_FAILED_INIT;
    if (curl && request_setup(curl, body, context->auth, &headers) == 0 &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, response_write_cb) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer) == CURLE_OK)
        performed = curl_easy_perform(curl);
    if (performed == CURLE_OK)
    {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status >= 200 && status < 300 && buffer.data) response = parse_response(buffer.data);
    }
    curl_slist_free_all(headers); curl_easy_cleanup(curl); free(buffer.data); free(body);
    return response;
}

static LLMResponse *openai_stream(LLMProvider *provider, Message *messages, int count,
                                  const char *model, double temperature, int timeout,
                                  void (*on_chunk)(const char *, void *), void *userdata,
                                  const char *tools_json)
{
    (void)temperature; (void)timeout;
    OpenAICtx *context = provider->ctx;
    char *body = build_request_body(messages, count, model, 1, tools_json);
    if (!body) return NULL;
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    LLMResponse *response = llm_response_create();
    StreamContext stream = {.response = response, .on_chunk = on_chunk, .userdata = userdata};
    CURLcode performed = CURLE_FAILED_INIT;
    if (curl && response && request_setup(curl, body, context->auth, &headers) == 0 &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream) == CURLE_OK)
        performed = curl_easy_perform(curl);
    if (performed != CURLE_OK || !response)
    { llm_response_free(response); response = NULL; }
    else
    {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status < 200 || status >= 300) { llm_response_free(response); response = NULL; }
        else if (!response->content) response->content = str_dup("");
    }
    free(stream.line); curl_slist_free_all(headers); curl_easy_cleanup(curl); free(body);
    return response;
}

static LLMResponse *openai_structured(LLMProvider *provider, Message *messages, int count,
                                       const char *model, double temperature, int timeout,
                                       const char *json_schema)
{
    (void)json_schema;
    return openai_chat(provider, messages, count, model, temperature, timeout, NULL);
}

static void openai_destroy(LLMProvider *provider)
{
    if (!provider) return;
    free(provider->ctx);
    free(provider);
}

LLMProvider *openai_provider_create(const char *base_url, const char *api_token,
                                    OpenAIOAuth *auth)
{
    (void)base_url; (void)api_token;
    LLMProvider *provider = calloc(1, sizeof(LLMProvider));
    OpenAICtx *context = calloc(1, sizeof(OpenAICtx));
    if (!provider || !context) { free(provider); free(context); return NULL; }
    context->auth = auth;
    provider->chat = openai_chat;
    provider->chat_streaming = openai_stream;
    provider->extract_structured = openai_structured;
    provider->destroy = openai_destroy;
    provider->ctx = context;
    return provider;
}
