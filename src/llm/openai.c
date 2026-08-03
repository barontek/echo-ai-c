#define _GNU_SOURCE
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <openssl/crypto.h>

#include "openai.h"
#include "openai_oauth.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

#define CODEX_ENDPOINT "https://chatgpt.com/backend-api/codex/responses"
#define CODEX_MODELS_ENDPOINT "https://chatgpt.com/backend-api/codex/models"
#define OPENAI_MAX_REQUEST_BYTES (16U * 1024U * 1024U)
#define OPENAI_MAX_RESPONSE_BYTES (32U * 1024U * 1024U)
#define OPENAI_MAX_SSE_EVENT_BYTES (4U * 1024U * 1024U)
#define OPENAI_MAX_MODELS_RESPONSE_BYTES (4U * 1024U * 1024U)
#define OPENAI_MAX_MODELS 512U
#define OPENAI_MAX_MODEL_NAME_BYTES 256U
#define OPENAI_MAX_TOKEN_BYTES 16384U
#define OPENAI_MAX_ACCOUNT_BYTES 2048U
#define OPENAI_ERROR_FIELD_BYTES 96U

#ifndef ECHO_AI_VERSION
#define ECHO_AI_VERSION "0.1.0"
#endif

/* The Codex backend filters its catalog by minimal_client_version, so this
 * must track a current official Codex CLI release, not Echo's own version. */
#define CODEX_CLIENT_VERSION "0.146.0"

typedef struct {
    OpenAIOAuth *auth;
    char *effort; /* owned; NULL = API default ("low"/"medium"/"high"/"xhigh"/"max"/"none") */
} OpenAICtx;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    size_t limit;
} Buffer;

typedef struct {
    char *token;
    char *account;
} Credentials;

typedef struct {
    int output_index;
    int response_index;
    char *item_id;
} FunctionCallMap;

typedef struct {
    LLMResponse *response;
    FunctionCallMap *calls;
    size_t calls_count;
    char *line;
    size_t line_len;
    size_t line_cap;
    Buffer event_data;
    void (*on_chunk)(const char *, void *);
    void *userdata;
    size_t received_bytes;
    int terminal_seen;
    int completed;
    int failed;
    int thinking_open;       /* 1 while a <think> block is open in content */
    int summary_deltas_seen; /* 1 after the first reasoning-summary delta */
} StreamParser;

typedef struct {
    CURL *curl;
    long status;
    Buffer error_body;
    StreamParser parser;
} LiveContext;

static void log_http_error(const char *operation, long status,
                           const char *body);

static void clear_secret(char **value)
{
    if (!value || !*value) return;
    memset(*value, 0, strlen(*value));
    free(*value);
    *value = NULL;
}

static void credentials_clear(Credentials *credentials)
{
    if (!credentials) return;
    clear_secret(&credentials->token);
    free(credentials->account);
    credentials->account = NULL;
}

static int buffer_append(Buffer *buffer, const void *bytes, size_t length)
{
    if (!buffer || (!bytes && length != 0)) return -1;
    if (length > buffer->limit || buffer->len > buffer->limit - length) return -1;
    if (buffer->len + length == SIZE_MAX) return -1;
    size_t needed = buffer->len + length + 1;
    if (needed > buffer->cap)
    {
        size_t capacity = buffer->cap ? buffer->cap : 1024U;
        while (capacity < needed)
        {
            if (capacity > (buffer->limit + 1U) / 2U)
            {
                capacity = buffer->limit + 1U;
                break;
            }
            capacity *= 2U;
        }
        if (capacity < needed) return -1;
        char *grown = realloc(buffer->data, capacity);
        if (!grown) return -1;
        buffer->data = grown;
        buffer->cap = capacity;
    }
    if (length != 0) memcpy(buffer->data + buffer->len, bytes, length);
    buffer->len += length;
    buffer->data[buffer->len] = '\0';
    return 0;
}

static int append_text(char **target, const char *value)
{
    if (!target || !value) return -1;
    size_t old_length = *target ? strlen(*target) : 0U;
    size_t add_length = strlen(value);
    if (add_length > SIZE_MAX - old_length - 1U) return -1;
    char *grown = realloc(*target, old_length + add_length + 1U);
    if (!grown) return -1;
    memcpy(grown + old_length, value, add_length + 1U);
    *target = grown;
    return 0;
}

static int json_add_item(cJSON *object, const char *name, cJSON *item)
{
    if (!object || !name || !item)
    {
        cJSON_Delete(item);
        return -1;
    }
    if (!cJSON_AddItemToObject(object, name, item))
    {
        cJSON_Delete(item);
        return -1;
    }
    return 0;
}

static int json_array_add(cJSON *array, cJSON *item)
{
    if (!array || !item)
    {
        cJSON_Delete(item);
        return -1;
    }
    if (!cJSON_AddItemToArray(array, item))
    {
        cJSON_Delete(item);
        return -1;
    }
    return 0;
}

static int json_add_string(cJSON *object, const char *name, const char *value)
{
    if (!value || strnlen(value, OPENAI_MAX_REQUEST_BYTES + 1U) >
                      OPENAI_MAX_REQUEST_BYTES)
        return -1;
    cJSON *string = value ? cJSON_CreateString(value) : NULL;
    return json_add_item(object, name, string);
}

static int json_add_bool(cJSON *object, const char *name, int value)
{
    return json_add_item(object, name, cJSON_CreateBool(value));
}

static int valid_nonempty_string(const cJSON *item)
{
    const char *value = cJSON_IsString(item) ? cJSON_GetStringValue(item) : NULL;
    return value && value[0] != '\0';
}

static int parse_bounded_json(const char *text, size_t limit, cJSON **json_out)
{
    if (!text || !json_out) return -1;
    *json_out = NULL;
    size_t length = strnlen(text, limit + 1U);
    if (length == 0U || length > limit) return -1;
    cJSON *json = cJSON_Parse(text);
    if (!json) return -1;
    *json_out = json;
    return 0;
}

static int append_instruction(char **instructions, const char *text)
{
    if (!text) return -1;
    size_t current = *instructions ? strlen(*instructions) : 0U;
    size_t added = strnlen(text, OPENAI_MAX_REQUEST_BYTES + 1U);
    if (added > OPENAI_MAX_REQUEST_BYTES ||
        current > OPENAI_MAX_REQUEST_BYTES - added ||
        (current != 0U && current + added == OPENAI_MAX_REQUEST_BYTES))
        return -1;
    if (*instructions && (*instructions)[0] != '\0' && append_text(instructions, "\n") != 0)
        return -1;
    return append_text(instructions, text);
}

static int add_message_content(cJSON *input, const char *role,
                               const char *content, const char *phase)
{
    cJSON *entry = cJSON_CreateObject();
    cJSON *parts = cJSON_CreateArray();
    cJSON *part = cJSON_CreateObject();
    const char *part_type = strcmp(role, "assistant") == 0
                                ? "output_text" : "input_text";
    if (!entry || !parts || !part)
    {
        cJSON_Delete(entry);
        cJSON_Delete(parts);
        cJSON_Delete(part);
        return -1;
    }
    if (json_add_string(entry, "role", role) != 0 ||
        (phase && strcmp(role, "assistant") == 0 &&
         json_add_string(entry, "phase", phase) != 0) ||
        json_add_string(part, "type", part_type) != 0 ||
        json_add_string(part, "text", content ? content : "") != 0)
    {
        cJSON_Delete(entry);
        cJSON_Delete(parts);
        cJSON_Delete(part);
        return -1;
    }
    if (json_array_add(parts, part) != 0)
    {
        cJSON_Delete(entry);
        cJSON_Delete(parts);
        return -1;
    }
    if (json_add_item(entry, "content", parts) != 0)
    {
        cJSON_Delete(entry);
        return -1;
    }
    return json_array_add(input, entry);
}

static int add_function_call_input(cJSON *input, const ToolCall *call)
{
    if (!call || !call->id || !call->id[0] || !call->name || !call->name[0] ||
        !call->arguments)
        return -1;
    cJSON *item = cJSON_CreateObject();
    if (!item) return -1;
    if (json_add_string(item, "type", "function_call") != 0 ||
        json_add_string(item, "call_id", call->id) != 0 ||
        json_add_string(item, "name", call->name) != 0 ||
        json_add_string(item, "arguments", call->arguments) != 0)
    {
        cJSON_Delete(item);
        return -1;
    }
    return json_array_add(input, item);
}

static int prior_function_call_exists(Message *messages, int before,
                                      const char *call_id)
{
    for (int i = 0; i < before; i++)
        for (int j = 0; j < messages[i].tool_calls_count; j++)
            if (messages[i].tool_calls && messages[i].tool_calls[j].id &&
                strcmp(messages[i].tool_calls[j].id, call_id) == 0)
                return 1;
    return 0;
}

static int function_call_id_seen(Message *messages, int message_index,
                                 int call_index, const char *call_id)
{
    for (int i = 0; i <= message_index; i++)
    {
        int limit = i == message_index ? call_index : messages[i].tool_calls_count;
        for (int j = 0; j < limit; j++)
            if (messages[i].tool_calls && messages[i].tool_calls[j].id &&
                strcmp(messages[i].tool_calls[j].id, call_id) == 0)
                return 1;
    }
    return 0;
}

static int prior_tool_output_exists(Message *messages, int before,
                                    const char *call_id)
{
    for (int i = 0; i < before; i++)
        if (messages[i].role && strcmp(messages[i].role, "tool") == 0 &&
            messages[i].tool_call_id &&
            strcmp(messages[i].tool_call_id, call_id) == 0)
            return 1;
    return 0;
}

static int add_tool_output(cJSON *input, Message *messages, int index)
{
    Message *message = &messages[index];
    if (!message->tool_call_id || !message->tool_call_id[0] || !message->content ||
        message->tool_calls_count != 0 ||
        !prior_function_call_exists(messages, index, message->tool_call_id) ||
        prior_tool_output_exists(messages, index, message->tool_call_id))
        return -1;
    cJSON *output = cJSON_CreateObject();
    if (!output) return -1;
    if (json_add_string(output, "type", "function_call_output") != 0 ||
        json_add_string(output, "call_id", message->tool_call_id) != 0 ||
        json_add_string(output, "output", message->content) != 0)
    {
        cJSON_Delete(output);
        return -1;
    }
    return json_array_add(input, output);
}

static int add_provider_state(cJSON *input, const char *provider_state)
{
    if (!provider_state) return 0;
    cJSON *items = NULL;
    if (parse_bounded_json(provider_state, OPENAI_MAX_REQUEST_BYTES, &items) != 0 ||
        !cJSON_IsArray(items))
    {
        cJSON_Delete(items);
        return -1;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!cJSON_IsObject(item) || !valid_nonempty_string(type) ||
            strcmp(cJSON_GetStringValue(type), "reasoning") != 0)
        { cJSON_Delete(items); return -1; }
        cJSON *copy = cJSON_Duplicate(item, 1);
        if (!copy)
        { cJSON_Delete(items); return -1; }
        if (json_array_add(input, copy) != 0)
        { cJSON_Delete(items); return -1; }
    }
    cJSON_Delete(items);
    return 0;
}

static int add_input_messages(cJSON *root, cJSON *input, Message *messages,
                              int count)
{
    char *instructions = NULL;
    for (int i = 0; i < count; i++)
    {
        Message *message = &messages[i];
        if (!message->role) goto fail;
        if (strcmp(message->role, "system") == 0 ||
            strcmp(message->role, "developer") == 0)
        {
            if (!message->content || message->tool_calls_count != 0 ||
                append_instruction(&instructions, message->content) != 0)
                goto fail;
            continue;
        }
        if (strcmp(message->role, "tool") == 0)
        {
            if (add_tool_output(input, messages, i) != 0) goto fail;
            continue;
        }
        if (strcmp(message->role, "user") != 0 &&
            strcmp(message->role, "assistant") != 0)
            goto fail;
        if (strcmp(message->role, "user") == 0 && message->tool_calls_count != 0)
            goto fail;
        if (strcmp(message->role, "assistant") == 0 &&
            add_provider_state(input, message->provider_state) != 0)
            goto fail;
        if (message->content && (message->content[0] != '\0' ||
                                 message->tool_calls_count == 0) &&
            add_message_content(input, message->role, message->content,
                                message->phase) != 0)
            goto fail;
        if (!message->content && message->tool_calls_count == 0) goto fail;
        if (message->tool_calls_count < 0 ||
            (message->tool_calls_count > 0 && !message->tool_calls))
            goto fail;
        for (int j = 0; j < message->tool_calls_count; j++)
            if (!message->tool_calls[j].id ||
                function_call_id_seen(messages, i, j,
                                      message->tool_calls[j].id) ||
                add_function_call_input(input, &message->tool_calls[j]) != 0)
                goto fail;
    }
    if (instructions && json_add_string(root, "instructions", instructions) != 0)
        goto fail;
    free(instructions);
    return 0;

fail:
    free(instructions);
    return -1;
}

static cJSON *convert_tools(const char *tools_json)
{
    cJSON *source = NULL;
    if (parse_bounded_json(tools_json, OPENAI_MAX_REQUEST_BYTES, &source) != 0 ||
        !cJSON_IsArray(source))
    {
        cJSON_Delete(source);
        return NULL;
    }
    cJSON *result = cJSON_CreateArray();
    if (!result) { cJSON_Delete(source); return NULL; }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, source)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        cJSON *function = cJSON_GetObjectItemCaseSensitive(item, "function");
        cJSON *name = function ? cJSON_GetObjectItemCaseSensitive(function, "name") : NULL;
        cJSON *description = function ? cJSON_GetObjectItemCaseSensitive(function, "description") : NULL;
        cJSON *parameters = function ? cJSON_GetObjectItemCaseSensitive(function, "parameters") : NULL;
        cJSON *strict = function ? cJSON_GetObjectItemCaseSensitive(function, "strict") : NULL;
        if (!cJSON_IsObject(item) || !valid_nonempty_string(type) ||
            strcmp(cJSON_GetStringValue(type), "function") != 0 ||
            !cJSON_IsObject(function) || !valid_nonempty_string(name) ||
            !cJSON_IsObject(parameters) ||
            (description && !cJSON_IsString(description)) ||
            (strict && !cJSON_IsBool(strict)))
            goto fail;
        cJSON *converted = cJSON_CreateObject();
        cJSON *parameters_copy = cJSON_Duplicate(parameters, 1);
        if (!converted || !parameters_copy)
        {
            cJSON_Delete(converted);
            cJSON_Delete(parameters_copy);
            goto fail;
        }
        if (json_add_string(converted, "type", "function") != 0 ||
            json_add_string(converted, "name", cJSON_GetStringValue(name)) != 0 ||
            (description && json_add_string(converted, "description",
                                             cJSON_GetStringValue(description)) != 0))
        {
            cJSON_Delete(parameters_copy);
            cJSON_Delete(converted);
            goto fail;
        }
        if (json_add_item(converted, "parameters", parameters_copy) != 0)
        {
            cJSON_Delete(converted);
            goto fail;
        }
        if (strict && json_add_bool(converted, "strict", cJSON_IsTrue(strict)) != 0)
        {
            cJSON_Delete(converted);
            goto fail;
        }
        if (json_array_add(result, converted) != 0) goto fail;
    }
    cJSON_Delete(source);
    return result;

fail:
    cJSON_Delete(source);
    cJSON_Delete(result);
    return NULL;
}

static int add_structured_format(cJSON *root, const char *json_schema)
{
    cJSON *schema = NULL;
    if (parse_bounded_json(json_schema, OPENAI_MAX_REQUEST_BYTES, &schema) != 0 ||
        !cJSON_IsObject(schema))
    {
        cJSON_Delete(schema);
        return -1;
    }
    cJSON *text = cJSON_CreateObject();
    cJSON *format = cJSON_CreateObject();
    if (!text || !format)
    {
        cJSON_Delete(schema);
        cJSON_Delete(text);
        cJSON_Delete(format);
        return -1;
    }
    if (json_add_string(format, "type", "json_schema") != 0 ||
        json_add_string(format, "name", "echo_structured_output") != 0)
    {
        cJSON_Delete(schema);
        cJSON_Delete(format);
        cJSON_Delete(text);
        return -1;
    }
    if (json_add_item(format, "schema", schema) != 0)
    {
        cJSON_Delete(format);
        cJSON_Delete(text);
        return -1;
    }
    if (json_add_bool(format, "strict", 1) != 0)
    {
        cJSON_Delete(format);
        cJSON_Delete(text);
        return -1;
    }
    if (json_add_item(text, "format", format) != 0)
    {
        cJSON_Delete(text);
        return -1;
    }
    return json_add_item(root, "text", text);
}

static int add_reasoning_include(cJSON *root)
{
    cJSON *include = cJSON_CreateArray();
    cJSON *item = cJSON_CreateString("reasoning.encrypted_content");
    if (!include || !item)
    {
        cJSON_Delete(include);
        cJSON_Delete(item);
        return -1;
    }
    if (!cJSON_AddItemToArray(include, item))
    {
        cJSON_Delete(item);
        cJSON_Delete(include);
        return -1;
    }
    return json_add_item(root, "include", include);
}

int openai_reasoning_effort_valid(const char *effort)
{
    if (!effort || !effort[0]) return 1;
    return strcmp(effort, "low") == 0 ||
           strcmp(effort, "medium") == 0 ||
           strcmp(effort, "high") == 0 ||
           strcmp(effort, "xhigh") == 0 ||
           strcmp(effort, "max") == 0 ||
           strcmp(effort, "none") == 0;
}

static int add_reasoning_config(cJSON *root, const char *effort)
{
    if (effort && !openai_reasoning_effort_valid(effort)) return -1;
    cJSON *reasoning = cJSON_CreateObject();
    if (!reasoning) return -1;
    /* The reasoning summary is the only readable form of Codex reasoning
     * (the chain of thought itself stays encrypted server-side); "auto"
     * selects the most detailed summary the model supports. Models that
     * don't support summaries accept and ignore the field. */
    if (json_add_string(reasoning, "summary", "auto") != 0 ||
        (effort && effort[0] &&
         json_add_string(reasoning, "effort", effort) != 0))
    {
        cJSON_Delete(reasoning);
        return -1;
    }
    return json_add_item(root, "reasoning", reasoning);
}

static char *build_request_body(Message *messages, int count, const char *model,
                                double temperature, int stream,
                                const char *tools_json,
                                const char *json_schema,
                                const char *effort)
{
    if ((!messages && count != 0) || count < 0 || !model || !model[0] ||
        !isfinite(temperature) || temperature < 0.0 || temperature > 2.0 ||
        (effort && !openai_reasoning_effort_valid(effort)))
        return NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *input = cJSON_CreateArray();
    if (!root || !input)
    {
        cJSON_Delete(root);
        cJSON_Delete(input);
        return NULL;
    }
    if (json_add_string(root, "model", model) != 0 ||
        json_add_bool(root, "stream", stream) != 0 ||
        json_add_bool(root, "store", 0) != 0 ||
        add_reasoning_include(root) != 0 ||
        add_reasoning_config(root, effort) != 0 ||
        add_input_messages(root, input, messages, count) != 0 ||
        cJSON_GetArraySize(input) == 0)
    {
        cJSON_Delete(root);
        cJSON_Delete(input);
        return NULL;
    }
    if (json_add_item(root, "input", input) != 0)
    {
        cJSON_Delete(root);
        return NULL;
    }
    if (tools_json && tools_json[0])
    {
        cJSON *tools = convert_tools(tools_json);
        if (!tools)
        {
            cJSON_Delete(root);
            return NULL;
        }
        if (cJSON_GetArraySize(tools) == 0)
            cJSON_Delete(tools);
        else if (json_add_item(root, "tools", tools) != 0)
        {
            cJSON_Delete(root);
            return NULL;
        }
        else if (json_add_bool(root, "parallel_tool_calls", 1) != 0)
        {
            cJSON_Delete(root);
            return NULL;
        }
    }
    if (json_schema && add_structured_format(root, json_schema) != 0)
    {
        cJSON_Delete(root);
        return NULL;
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return NULL;
    if (strnlen(body, OPENAI_MAX_REQUEST_BYTES + 1U) > OPENAI_MAX_REQUEST_BYTES)
    {
        free(body);
        return NULL;
    }
    return body;
}

static int valid_header_value(const char *value, size_t maximum)
{
    if (!value || !value[0]) return 0;
    size_t length = strnlen(value, maximum + 1U);
    return length <= maximum && !strchr(value, '\r') && !strchr(value, '\n');
}

static int header_append(struct curl_slist **headers, const char *value)
{
    struct curl_slist *grown = curl_slist_append(*headers, value);
    if (!grown) return -1;
    *headers = grown;
    return 0;
}

static void headers_free(struct curl_slist *headers)
{
    for (struct curl_slist *item = headers; item; item = item->next)
        if (item->data && strncmp(item->data, "Authorization: Bearer ", 22) == 0)
            OPENSSL_cleanse(item->data, strlen(item->data));
    curl_slist_free_all(headers);
}

static int build_headers(const Credentials *credentials,
                         struct curl_slist **headers_out)
{
    if (!credentials || !headers_out ||
        !valid_header_value(credentials->token, OPENAI_MAX_TOKEN_BYTES) ||
        (credentials->account &&
         !valid_header_value(credentials->account, OPENAI_MAX_ACCOUNT_BYTES)))
        return -1;
    *headers_out = NULL;
    char authorization[OPENAI_MAX_TOKEN_BYTES + 32U] = {0};
    int written = snprintf(authorization, sizeof(authorization),
                           "Authorization: Bearer %s", credentials->token);
    if (written < 0 || (size_t)written >= sizeof(authorization))
    { OPENSSL_cleanse(authorization, sizeof(authorization)); return -1; }
    int header_result = header_append(headers_out, "Content-Type: application/json") == 0 &&
        header_append(headers_out, authorization) == 0 &&
        header_append(headers_out, "originator: echo-ai") == 0 &&
        header_append(headers_out, "User-Agent: echo-ai") == 0;
    OPENSSL_cleanse(authorization, sizeof(authorization));
    if (!header_result)
        goto fail;
    if (credentials->account)
    {
        char account_header[OPENAI_MAX_ACCOUNT_BYTES + 32U] = {0};
        written = snprintf(account_header, sizeof(account_header),
                           "ChatGPT-Account-Id: %s", credentials->account);
        int account_result = written >= 0 && (size_t)written < sizeof(account_header) &&
                             header_append(headers_out, account_header) == 0;
        OPENSSL_cleanse(account_header, sizeof(account_header));
        if (!account_result)
            goto fail;
    }
    return 0;

fail:
    headers_free(*headers_out);
    *headers_out = NULL;
    return -1;
}

static int request_setup(CURL *curl, const char *body, int timeout,
                         const Credentials *credentials,
                         struct curl_slist **headers_out)
{
    if (!curl || !body || timeout <= 0 ||
        strnlen(body, OPENAI_MAX_REQUEST_BYTES + 1U) > OPENAI_MAX_REQUEST_BYTES ||
        build_headers(credentials, headers_out) != 0)
        return -1;
    if (curl_easy_setopt(curl, CURLOPT_URL, CODEX_ENDPOINT) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POST, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body)) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *headers_out) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK)
    {
        headers_free(*headers_out);
        *headers_out = NULL;
        return -1;
    }
    return 0;
}

static int credentials_get(OpenAIOAuth *auth, Credentials *credentials)
{
    if (!credentials ||
        openai_oauth_get_access_token(auth, &credentials->token,
                                      &credentials->account) != 0 ||
        !valid_header_value(credentials->token, OPENAI_MAX_TOKEN_BYTES) ||
        (credentials->account &&
         !valid_header_value(credentials->account, OPENAI_MAX_ACCOUNT_BYTES)))
    {
        credentials_clear(credentials);
        return -1;
    }
    return 0;
}

static int credentials_refresh_401(OpenAIOAuth *auth,
                                   Credentials *credentials)
{
    char *access_token = NULL;
    char *account_id = NULL;
    if (!auth || !credentials || !credentials->token ||
        openai_oauth_refresh_after_401(auth, credentials->token,
                                       &access_token, &account_id) !=
            OPENAI_OAUTH_TOKEN_OK)
    {
        clear_secret(&access_token);
        free(account_id);
        return -1;
    }
    if (!valid_header_value(access_token, OPENAI_MAX_TOKEN_BYTES) ||
        (account_id && !valid_header_value(account_id, OPENAI_MAX_ACCOUNT_BYTES)))
    {
        clear_secret(&access_token);
        free(account_id);
        return -1;
    }
    credentials_clear(credentials);
    credentials->token = access_token;
    credentials->account = account_id;
    return 0;
}

void openai_models_free(char **models, size_t count)
{
    if (!models) return;
    for (size_t i = 0; i < count; i++) free(models[i]);
    free(models);
}

static int model_already_added(char **models, size_t count, const char *slug)
{
    for (size_t i = 0; i < count; i++)
        if (strcmp(models[i], slug) == 0) return 1;
    return 0;
}

static void log_empty_catalog_diagnostic(const char *body)
{
    cJSON *root = NULL;
    if (!body || parse_bounded_json(body, OPENAI_MAX_MODELS_RESPONSE_BYTES,
                                    &root) != 0 || !cJSON_IsObject(root))
    { cJSON_Delete(root); return; }
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "models");
    int total = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    int list_visible = 0;
    char vis_buf[64] = {0};
    char *write = vis_buf;
    if (cJSON_IsArray(items))
    {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, items)
        {
            cJSON *vis_json = cJSON_GetObjectItemCaseSensitive(item, "visibility");
            const char *vis = cJSON_IsString(vis_json) ?
                cJSON_GetStringValue(vis_json) : "<non-string>";
            if (strcmp(vis, "list") == 0) list_visible++;
            if (write - vis_buf < (long)sizeof(vis_buf) - 2L)
            {
                if (write != vis_buf) *write++ = ',';
                int copied = snprintf(write,
                    (size_t)(vis_buf + sizeof(vis_buf) - write), "%s", vis);
                if (copied > 0 &&
                    (size_t)copied < (size_t)(vis_buf + sizeof(vis_buf) - write))
                    write += copied;
            }
        }
    }
    char total_buf[16];
    char list_buf[16];
    snprintf(total_buf, sizeof(total_buf), "%d", total);
    snprintf(list_buf, sizeof(list_buf), "%d", list_visible);
    log_warn("OpenAI Codex model catalog contained no list-visible models",
             "items", total_buf, "list_visible", list_buf,
             "visibility_values", vis_buf, NULL);
    cJSON_Delete(root);
}

static int parse_models_response(const char *raw, char ***models_out,
                                 size_t *count_out)
{

    if (!models_out || !count_out) return -1;
    *models_out = NULL;
    *count_out = 0U;
    cJSON *root = NULL;
    if (parse_bounded_json(raw, OPENAI_MAX_MODELS_RESPONSE_BYTES, &root) != 0 ||
        !cJSON_IsObject(root))
    { cJSON_Delete(root); return -1; }
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "models");
    int item_count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : -1;
    if (item_count < 0 || (size_t)item_count > OPENAI_MAX_MODELS)
    { cJSON_Delete(root); return -1; }
    char **models = item_count > 0 ?
        calloc((size_t)item_count, sizeof(*models)) : NULL;
    if (item_count > 0 && !models) { cJSON_Delete(root); return -1; }
    size_t count = 0U;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items)
    {
        cJSON *slug_json = cJSON_GetObjectItemCaseSensitive(item, "slug");
        cJSON *visibility_json = cJSON_GetObjectItemCaseSensitive(item, "visibility");
        const char *slug = cJSON_IsString(slug_json) ?
            cJSON_GetStringValue(slug_json) : NULL;
        const char *visibility = cJSON_IsString(visibility_json) ?
            cJSON_GetStringValue(visibility_json) : NULL;
        if (!cJSON_IsObject(item) || !slug || !slug[0] || !visibility ||
            strnlen(slug, OPENAI_MAX_MODEL_NAME_BYTES + 1U) >
                OPENAI_MAX_MODEL_NAME_BYTES)
            continue;
        if (strcmp(visibility, "list") != 0 ||
            model_already_added(models, count, slug))
            continue;
        models[count] = str_dup(slug);
        if (!models[count]) goto fail;
        count++;
    }
    cJSON_Delete(root);
    if (count == 0U)
    {
        /* Empty catalog: keep the "count 0 means NULL" contract. */
        free(models);
        models = NULL;
    }
    *models_out = models;
    *count_out = count;
    return 0;

fail:
    cJSON_Delete(root);
    openai_models_free(models, count);
    return -1;
}

static size_t models_write_cb(void *ptr, size_t size, size_t nmemb,
                              void *userdata)
{
    if (size != 0U && nmemb > SIZE_MAX / size) return 0U;
    size_t total = size * nmemb;
    return buffer_append(userdata, ptr, total) == 0 ? total : 0U;
}

static int models_request_once(const Credentials *credentials, char **body_out,
                               long *status_out)
{
    *body_out = NULL;
    *status_out = 0L;
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    Buffer body = {.limit = OPENAI_MAX_MODELS_RESPONSE_BYTES};
    char url[512] = {0};
    char version_header[128] = {0};
    int url_len = snprintf(url, sizeof(url), "%s?client_version=%s",
                           CODEX_MODELS_ENDPOINT, CODEX_CLIENT_VERSION);
    int header_len = snprintf(version_header, sizeof(version_header),
                              "version: %s", CODEX_CLIENT_VERSION);
    int setup_ok = curl && url_len > 0 && (size_t)url_len < sizeof(url) &&
        header_len > 0 && (size_t)header_len < sizeof(version_header) &&
        build_headers(credentials, &headers) == 0 &&
        header_append(&headers, version_header) == 0 &&
        curl_easy_setopt(curl, CURLOPT_URL, url) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, models_write_cb) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body) == CURLE_OK;
    CURLcode performed = setup_ok ? curl_easy_perform(curl) : CURLE_FAILED_INIT;
    if (performed == CURLE_OK &&
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status_out) != CURLE_OK)
        performed = CURLE_GOT_NOTHING;
    headers_free(headers);
    if (curl) curl_easy_cleanup(curl);
    if (performed != CURLE_OK)
    {
        free(body.data);
        return -1;
    }
    *body_out = body.data;
    return 0;
}

int openai_models_fetch_alloc(OpenAIOAuth *auth, char ***models_out,
                              size_t *count_out)
{
    if (!auth || !models_out || !count_out) return -1;
    *models_out = NULL;
    *count_out = 0U;
    Credentials credentials = {0};
    if (credentials_get(auth, &credentials) != 0) return OPENAI_MODELS_UNAVAILABLE;
    int result = OPENAI_MODELS_UNAVAILABLE;
    int denied = 0;
    for (int attempt = 0; attempt < 2; attempt++)
    {
        char *body = NULL;
        long status = 0L;
        if (models_request_once(&credentials, &body, &status) != 0)
        { log_error("OpenAI Codex models request failed", NULL); free(body); break; }
        if (status == 401L && attempt == 0 &&
            credentials_refresh_401(auth, &credentials) == 0)
        { free(body); continue; }
        if (status < 200L || status >= 300L)
        {
            log_http_error("models", status, body);
            if (status >= 400L && status < 500L) denied = 1;
        }
        else
        {
            result = parse_models_response(body, models_out, count_out);
            if (result == 0 && *count_out == 0U)
                log_empty_catalog_diagnostic(body);
            if (result != 0 || *count_out == 0U)
            {
                char snippet[257] = {0};
                if (body) memcpy(snippet, body, sizeof(snippet) - 1U);
                log_debug("OpenAI Codex models response", "body", snippet, NULL);
            }
        }
        free(body);
        break;
    }
    credentials_clear(&credentials);
    return denied ? OPENAI_MODELS_DENIED : result;
}

static void copy_safe_error_field(char output[OPENAI_ERROR_FIELD_BYTES],
                                  const cJSON *field)
{
    const char *value = cJSON_IsString(field) ? cJSON_GetStringValue(field) : NULL;
    size_t used = 0U;
    if (!value) { output[0] = '\0'; return; }
    while (*value && used + 1U < OPENAI_ERROR_FIELD_BYTES)
    {
        unsigned char byte = (unsigned char)*value++;
        output[used++] = (char)(isalnum(byte) || byte == '.' || byte == '_' ||
                                byte == '-' ? byte : '_');
    }
    output[used] = '\0';
}

static void log_http_error(const char *operation, long status,
                           const char *body)
{
    char status_text[32] = {0};
    char type[OPENAI_ERROR_FIELD_BYTES] = {0};
    char code[OPENAI_ERROR_FIELD_BYTES] = {0};
    int written = snprintf(status_text, sizeof(status_text), "%ld", status);
    if (written < 0 || (size_t)written >= sizeof(status_text))
        memcpy(status_text, "unknown", sizeof("unknown"));
    cJSON *root = NULL;
    if (body && parse_bounded_json(body, OPENAI_MAX_RESPONSE_BYTES, &root) == 0)
    {
        cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (!cJSON_IsObject(error)) error = root;
        copy_safe_error_field(type,
            cJSON_GetObjectItemCaseSensitive(error, "type"));
        copy_safe_error_field(code,
            cJSON_GetObjectItemCaseSensitive(error, "code"));
    }
    log_error("OpenAI Codex request returned non-2xx", "operation", operation,
              "status", status_text, "error_type", type[0] ? type : "unknown",
              "error_code", code[0] ? code : "unknown", NULL);
    cJSON_Delete(root);
}

static int add_response_tool_call(LLMResponse *response, const char *id,
                                  const char *name, const char *arguments,
                                  int *index_out)
{
    if (!response || response->tool_calls_count < 0 ||
        response->tool_calls_count == INT_MAX)
        return -1;
    char *id_copy = str_dup(id ? id : "");
    char *name_copy = str_dup(name ? name : "");
    char *arguments_copy = str_dup(arguments ? arguments : "");
    if (!id_copy || !name_copy || !arguments_copy)
    {
        free(id_copy);
        free(name_copy);
        free(arguments_copy);
        return -1;
    }
    size_t count = (size_t)response->tool_calls_count + 1U;
    if (count > SIZE_MAX / sizeof(*response->tool_calls))
    {
        free(id_copy);
        free(name_copy);
        free(arguments_copy);
        return -1;
    }
    ToolCall *grown = realloc(response->tool_calls,
                              count * sizeof(*response->tool_calls));
    if (!grown)
    {
        free(id_copy);
        free(name_copy);
        free(arguments_copy);
        return -1;
    }
    response->tool_calls = grown;
    ToolCall *call = &grown[response->tool_calls_count];
    memset(call, 0, sizeof(*call));
    call->id = id_copy;
    call->name = name_copy;
    call->arguments = arguments_copy;
    if (index_out) *index_out = response->tool_calls_count;
    response->tool_calls_count++;
    return 0;
}

#ifdef OPENAI_TEST
static int parse_function_call_item(LLMResponse *response, const cJSON *item)
{
    cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "call_id");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
    cJSON *arguments = cJSON_GetObjectItemCaseSensitive(item, "arguments");
    if (!valid_nonempty_string(id) || !valid_nonempty_string(name) ||
        !cJSON_IsString(arguments))
        return -1;
    return add_response_tool_call(response, cJSON_GetStringValue(id),
                                  cJSON_GetStringValue(name),
                                  cJSON_GetStringValue(arguments), NULL);
}

static int parse_message_item(LLMResponse *response, const cJSON *item)
{
    cJSON *phase = cJSON_GetObjectItemCaseSensitive(item, "phase");
    if (phase)
    {
        if (!valid_nonempty_string(phase)) return -1;
        char *copy = str_dup(cJSON_GetStringValue(phase));
        if (!copy) return -1;
        free(response->phase);
        response->phase = copy;
    }
    cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
    if (!cJSON_IsArray(content)) return -1;
    cJSON *part = NULL;
    cJSON_ArrayForEach(part, content)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(part, "type");
        if (!valid_nonempty_string(type)) return -1;
        const char *type_value = cJSON_GetStringValue(type);
        cJSON *text = strcmp(type_value, "refusal") == 0
                          ? cJSON_GetObjectItemCaseSensitive(part, "refusal")
                          : cJSON_GetObjectItemCaseSensitive(part, "text");
        if (strcmp(type_value, "output_text") != 0 &&
            strcmp(type_value, "refusal") != 0)
            continue;
        if (!cJSON_IsString(text) ||
            append_text(&response->content, cJSON_GetStringValue(text)) != 0)
            return -1;
    }
    return 0;
}
#endif

static int response_status_ok(const cJSON *root)
{
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (error && !cJSON_IsNull(error)) return 0;
    if (!status) return 1;
    return cJSON_IsString(status) &&
           strcmp(cJSON_GetStringValue(status), "completed") == 0;
}

#ifdef OPENAI_TEST
/* defined below parse_response; declared here so the buffered parse can
 * reuse the streaming path's summary extraction */
static char *reasoning_summary_join(const cJSON *item);

static LLMResponse *parse_response(const char *raw)
{
    cJSON *root = NULL;
    if (parse_bounded_json(raw, OPENAI_MAX_RESPONSE_BYTES, &root) != 0 ||
        !cJSON_IsObject(root) || !response_status_ok(root))
    {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *output = cJSON_GetObjectItemCaseSensitive(root, "output");
    if (!cJSON_IsArray(output)) { cJSON_Delete(root); return NULL; }
    LLMResponse *response = llm_response_create();
    if (!response) { cJSON_Delete(root); return NULL; }
    char *reasoning_text = NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, output)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!valid_nonempty_string(type)) goto fail;
        const char *value = cJSON_GetStringValue(type);
        if (strcmp(value, "message") == 0)
        {
            if (parse_message_item(response, item) != 0) goto fail;
        }
        else if (strcmp(value, "function_call") == 0)
        {
            if (parse_function_call_item(response, item) != 0) goto fail;
        }
        else if (strcmp(value, "reasoning") == 0)
        {
            char *summary = reasoning_summary_join(item);
            if (summary)
            {
                if ((reasoning_text && append_text(&reasoning_text, "\n") != 0) ||
                    append_text(&reasoning_text, summary) != 0)
                { free(summary); goto fail; }
                free(summary);
            }
        }
    }
    if (!response->content && append_text(&response->content, "") != 0) goto fail;
    if (reasoning_text && reasoning_text[0])
    {
        char *tagged = NULL;
        if (asprintf(&tagged, "<think>\n%s\n</think>\n\n%s", reasoning_text,
                     response->content ? response->content : "") < 0)
        { free(reasoning_text); goto fail; }
        free(response->content);
        response->content = tagged;
        response->thinking = reasoning_text; /* ownership transferred */
    }
    else
    {
        free(reasoning_text);
    }
    cJSON_Delete(root);
    return response;

fail:
    cJSON_Delete(root);
    llm_response_free(response);
    return NULL;
}
#endif

static int json_integer(const cJSON *item, int *value_out)
{
    if (!cJSON_IsNumber(item) || !value_out || !isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 || item->valuedouble > (double)INT_MAX ||
        floor(item->valuedouble) != item->valuedouble)
        return -1;
    *value_out = (int)item->valuedouble;
    return 0;
}

static int map_matches(const StreamParser *parser, size_t index,
                       int output_index, const char *item_id,
                       const char *call_id)
{
    const FunctionCallMap *map = &parser->calls[index];
    const ToolCall *call = &parser->response->tool_calls[map->response_index];
    return (output_index >= 0 && map->output_index == output_index) ||
           (item_id && map->item_id && strcmp(map->item_id, item_id) == 0) ||
           (call_id && call->id && call->id[0] && strcmp(call->id, call_id) == 0);
}

static int find_stream_call(const StreamParser *parser, int output_index,
                            const char *item_id, const char *call_id)
{
    for (size_t i = 0; i < parser->calls_count; i++)
        if (map_matches(parser, i, output_index, item_id, call_id))
            return (int)i;
    return -1;
}

static int add_stream_call(StreamParser *parser, int output_index,
                           const char *item_id, const char *call_id,
                           const char *name, const char *arguments)
{
    if (parser->calls_count >= (size_t)INT_MAX) return -1;
    char *item_id_copy = item_id ? str_dup(item_id) : NULL;
    if (item_id && !item_id_copy) return -1;
    int response_index = -1;
    if (add_response_tool_call(parser->response, call_id, name, arguments,
                               &response_index) != 0)
    {
        free(item_id_copy);
        return -1;
    }
    size_t count = parser->calls_count + 1U;
    if (count > SIZE_MAX / sizeof(*parser->calls))
    {
        free(item_id_copy);
        return -1;
    }
    FunctionCallMap *grown = realloc(parser->calls,
                                     count * sizeof(*parser->calls));
    if (!grown)
    {
        free(item_id_copy);
        return -1;
    }
    parser->calls = grown;
    grown[parser->calls_count].output_index = output_index;
    grown[parser->calls_count].response_index = response_index;
    grown[parser->calls_count].item_id = item_id_copy;
    parser->calls_count = count;
    return (int)(count - 1U);
}

static int replace_call_field(char **field, const char *value, int require_empty)
{
    if (!value) return 0;
    if (*field && (*field)[0] && require_empty && strcmp(*field, value) != 0)
        return -1;
    char *copy = str_dup(value);
    if (!copy) return -1;
    free(*field);
    *field = copy;
    return 0;
}

static int upsert_function_item(StreamParser *parser, int output_index,
                                const cJSON *item)
{
    cJSON *item_id_json = cJSON_GetObjectItemCaseSensitive(item, "id");
    cJSON *call_id_json = cJSON_GetObjectItemCaseSensitive(item, "call_id");
    cJSON *name_json = cJSON_GetObjectItemCaseSensitive(item, "name");
    cJSON *arguments_json = cJSON_GetObjectItemCaseSensitive(item, "arguments");
    const char *item_id = cJSON_IsString(item_id_json)
                              ? cJSON_GetStringValue(item_id_json) : NULL;
    const char *call_id = cJSON_IsString(call_id_json)
                              ? cJSON_GetStringValue(call_id_json) : NULL;
    const char *name = cJSON_IsString(name_json)
                           ? cJSON_GetStringValue(name_json) : NULL;
    const char *arguments = cJSON_IsString(arguments_json)
                                ? cJSON_GetStringValue(arguments_json) : NULL;
    int map_index = find_stream_call(parser, output_index, item_id, call_id);
    if (map_index < 0)
        return add_stream_call(parser, output_index, item_id, call_id, name,
                               arguments) < 0 ? -1 : 0;
    FunctionCallMap *map = &parser->calls[map_index];
    ToolCall *call = &parser->response->tool_calls[map->response_index];
    if (output_index >= 0 && map->output_index >= 0 &&
        map->output_index != output_index)
        return -1;
    if (map->output_index < 0) map->output_index = output_index;
    if (item_id && replace_call_field(&map->item_id, item_id, 1) != 0) return -1;
    if (call_id && replace_call_field(&call->id, call_id, 1) != 0) return -1;
    if (name && replace_call_field(&call->name, name, 1) != 0) return -1;
    if (arguments && replace_call_field(&call->arguments, arguments, 0) != 0)
        return -1;
    return 0;
}

static int event_output_index(const cJSON *event)
{
    cJSON *index = cJSON_GetObjectItemCaseSensitive(event, "output_index");
    int value = -1;
    return index && json_integer(index, &value) == 0 ? value : -1;
}

static int capture_message_phase(LLMResponse *response, const cJSON *item)
{
    cJSON *phase = cJSON_GetObjectItemCaseSensitive(item, "phase");
    if (!phase) return 0;
    if (!valid_nonempty_string(phase)) return -1;
    char *copy = str_dup(cJSON_GetStringValue(phase));
    if (!copy) return -1;
    free(response->phase);
    response->phase = copy;
    return 0;
}

static int parse_argument_delta(StreamParser *parser, const cJSON *event,
                                int done)
{
    cJSON *item_id_json = cJSON_GetObjectItemCaseSensitive(event, "item_id");
    cJSON *call_id_json = cJSON_GetObjectItemCaseSensitive(event, "call_id");
    cJSON *value_json = cJSON_GetObjectItemCaseSensitive(
        event, done ? "arguments" : "delta");
    const char *item_id = cJSON_IsString(item_id_json)
                              ? cJSON_GetStringValue(item_id_json) : NULL;
    const char *call_id = cJSON_IsString(call_id_json)
                              ? cJSON_GetStringValue(call_id_json) : NULL;
    const char *value = cJSON_IsString(value_json)
                            ? cJSON_GetStringValue(value_json) : NULL;
    if (!value) return -1;
    int output_index = event_output_index(event);
    int map_index = find_stream_call(parser, output_index, item_id, call_id);
    if (map_index < 0)
    {
        map_index = add_stream_call(parser, output_index, item_id, call_id,
                                    NULL, NULL);
        if (map_index < 0) return -1;
    }
    ToolCall *call = &parser->response->tool_calls[
        parser->calls[map_index].response_index];
    return done ? replace_call_field(&call->arguments, value, 0)
                : append_text(&call->arguments, value);
}

static int merge_completed_output(StreamParser *parser, const cJSON *response)
{
    cJSON *output = cJSON_GetObjectItemCaseSensitive(response, "output");
    if (!output) return 0;
    if (!cJSON_IsArray(output)) return -1;
    cJSON *reasoning = cJSON_CreateArray();
    if (!reasoning) return -1;
    int index = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, output)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!valid_nonempty_string(type))
        { cJSON_Delete(reasoning); return -1; }
        if (strcmp(cJSON_GetStringValue(type), "reasoning") == 0)
        {
            cJSON *copy = cJSON_Duplicate(item, 1);
            if (!copy || !cJSON_AddItemToArray(reasoning, copy))
            { cJSON_Delete(copy); cJSON_Delete(reasoning); return -1; }
        }
        else if (strcmp(cJSON_GetStringValue(type), "function_call") == 0 &&
            upsert_function_item(parser, index, item) != 0)
        { cJSON_Delete(reasoning); return -1; }
        else if (strcmp(cJSON_GetStringValue(type), "message") == 0 &&
                 capture_message_phase(parser->response, item) != 0)
        { cJSON_Delete(reasoning); return -1; }
        index++;
    }
    char *serialized = cJSON_GetArraySize(reasoning) > 0 ?
        cJSON_PrintUnformatted(reasoning) : NULL;
    int had_reasoning = cJSON_GetArraySize(reasoning) > 0;
    cJSON_Delete(reasoning);
    if (had_reasoning && !serialized) return -1;
    if (had_reasoning)
    {
        free(parser->response->provider_state);
        parser->response->provider_state = serialized;
    }
    return 0;
}

static int append_reasoning_item(StreamParser *parser, const cJSON *item)
{
    cJSON *reasoning = NULL;
    if (parser->response->provider_state)
    {
        if (parse_bounded_json(parser->response->provider_state,
                               OPENAI_MAX_REQUEST_BYTES, &reasoning) != 0 ||
            !cJSON_IsArray(reasoning))
        {
            cJSON_Delete(reasoning);
            return -1;
        }
    }
    else
    {
        reasoning = cJSON_CreateArray();
        if (!reasoning) return -1;
    }
    cJSON *copy = cJSON_Duplicate(item, 1);
    if (!copy || !cJSON_AddItemToArray(reasoning, copy))
    {
        cJSON_Delete(copy);
        cJSON_Delete(reasoning);
        return -1;
    }
    char *serialized = cJSON_PrintUnformatted(reasoning);
    cJSON_Delete(reasoning);
    if (!serialized) return -1;
    free(parser->response->provider_state);
    parser->response->provider_state = serialized;
    return 0;
}

/* Opens (on first use) a <think> block, forwards the text via on_chunk,
 * and accumulates it into response->content so the saved message keeps the
 * tags — same convention as the ollama provider. */
static int emit_thinking_text(StreamParser *parser, const char *text)
{
    if (!text || !text[0]) return 0;
    if (!parser->thinking_open)
    {
        if (append_text(&parser->response->content, "<think>\n") != 0) return -1;
        if (parser->on_chunk) parser->on_chunk("<think>\n", parser->userdata);
        parser->thinking_open = 1;
    }
    if (append_text(&parser->response->content, text) != 0) return -1;
    if (parser->on_chunk) parser->on_chunk(text, parser->userdata);
    return 0;
}

static int close_thinking_block(StreamParser *parser)
{
    if (!parser->thinking_open) return 0;
    if (append_text(&parser->response->content, "\n</think>\n\n") != 0) return -1;
    if (parser->on_chunk) parser->on_chunk("\n</think>\n\n", parser->userdata);
    parser->thinking_open = 0;
    return 0;
}

/* Joins the text entries of a reasoning item's summary array (each entry
 * is {"type":"summary_text","text":"..."}) into a caller-owned string.
 * Returns NULL when the item has no readable summary. */
static char *reasoning_summary_join(const cJSON *item)
{
    cJSON *summary = cJSON_GetObjectItemCaseSensitive(item, "summary");
    if (!cJSON_IsArray(summary)) return NULL;
    char *joined = NULL;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, summary)
    {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(entry, "text");
        if (!cJSON_IsString(text)) continue;
        const char *part = cJSON_GetStringValue(text);
        if (!part[0]) continue;
        if (joined == NULL)
        {
            joined = str_dup(part);
            if (!joined) return NULL;
        }
        else
        {
            if (append_text(&joined, "\n") != 0 ||
                append_text(&joined, part) != 0)
            { free(joined); return NULL; }
        }
    }
    return joined;
}

/* Codex can return a literal HTML comment ("<!-- -->") as reasoning
 * summary text instead of real content (server-side regression, tracked
 * as openai/codex#31664). Strip the markers so the thinking block never
 * renders an empty placeholder; a comment-only summary emits nothing. */
static int emit_summary_text(StreamParser *parser, const char *text)
{
    if (!text || !text[0]) return 0;
    if (!strstr(text, "<!--") && !strstr(text, "-->"))
        return emit_thinking_text(parser, text);
    char *clean = str_dup(text);
    if (!clean) return -1;
    char *src = clean;
    char *dst = clean;
    while (*src)
    {
        if (strncmp(src, "<!--", 4) == 0) { src += 4; continue; }
        if (strncmp(src, "-->", 3) == 0) { src += 3; continue; }
        *dst++ = *src++;
    }
    *dst = '\0';
    int rc = 0;
    if (clean[0] != '\0')
    {
        const char *p = clean;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '\0') rc = emit_thinking_text(parser, clean);
    }
    free(clean);
    return rc;
}

static int parse_stream_event(StreamParser *parser, const char *text)
{
    cJSON *event = NULL;
    if (parse_bounded_json(text, OPENAI_MAX_SSE_EVENT_BYTES, &event) != 0 ||
        !cJSON_IsObject(event))
    {
        cJSON_Delete(event);
        return -1;
    }
    cJSON *type_json = cJSON_GetObjectItemCaseSensitive(event, "type");
    if (!valid_nonempty_string(type_json)) { cJSON_Delete(event); return -1; }
    const char *type = cJSON_GetStringValue(type_json);
    int result = 0;
    if (strcmp(type, "response.output_text.delta") == 0)
    {
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(event, "delta");
        if (!cJSON_IsString(delta) ||
            close_thinking_block(parser) != 0 ||
            append_text(&parser->response->content,
                        cJSON_GetStringValue(delta)) != 0)
            result = -1;
        else if (parser->on_chunk)
            parser->on_chunk(cJSON_GetStringValue(delta), parser->userdata);
    }
    else if (strcmp(type, "response.reasoning_summary_text.delta") == 0)
    {
        /* Plaintext summary of the model's reasoning, streamed as the
         * model thinks; this is the readable stand-in for the encrypted
         * chain of thought. */
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(event, "delta");
        if (!cJSON_IsString(delta) ||
            emit_summary_text(parser, cJSON_GetStringValue(delta)) != 0)
            result = -1;
        else
            parser->summary_deltas_seen = 1;
    }
    else if (strcmp(type, "response.reasoning_summary_text.done") == 0)
    {
        /* Carries the full summary; skip it when the deltas already
         * streamed it, or appending again would duplicate the block. */
        cJSON *text = cJSON_GetObjectItemCaseSensitive(event, "text");
        if (!parser->summary_deltas_seen && cJSON_IsString(text) &&
            emit_summary_text(parser, cJSON_GetStringValue(text)) != 0)
            result = -1;
    }
    else if (strcmp(type, "response.output_item.added") == 0 ||
             strcmp(type, "response.output_item.done") == 0)
    {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(event, "item");
        cJSON *item_type = item ? cJSON_GetObjectItemCaseSensitive(item, "type") : NULL;
        if (!cJSON_IsObject(item) || !valid_nonempty_string(item_type)) result = -1;
        else if (strcmp(cJSON_GetStringValue(item_type), "function_call") == 0)
            result = upsert_function_item(parser, event_output_index(event), item);
        else if (strcmp(type, "response.output_item.done") == 0 &&
                 strcmp(cJSON_GetStringValue(item_type), "reasoning") == 0)
        {
            result = append_reasoning_item(parser, item);
            /* Backends that never emit summary deltas still carry the final
             * summary on the reasoning item itself. */
            if (result == 0 && !parser->thinking_open)
            {
                char *summary = reasoning_summary_join(item);
                if (summary)
                {
                    result = emit_summary_text(parser, summary);
                    free(summary);
                }
            }
            if (result == 0) result = close_thinking_block(parser);
            parser->summary_deltas_seen = 0;
        }
        else if (strcmp(type, "response.output_item.done") == 0 &&
                 strcmp(cJSON_GetStringValue(item_type), "message") == 0)
            result = capture_message_phase(parser->response, item);
    }
    else if (strcmp(type, "response.function_call_arguments.delta") == 0)
        result = parse_argument_delta(parser, event, 0);
    else if (strcmp(type, "response.function_call_arguments.done") == 0)
        result = parse_argument_delta(parser, event, 1);
    else if (strcmp(type, "response.completed") == 0)
    {
        cJSON *response = cJSON_GetObjectItemCaseSensitive(event, "response");
        parser->terminal_seen = 1;
        parser->completed = cJSON_IsObject(response) && response_status_ok(response);
        if (!parser->completed || merge_completed_output(parser, response) != 0)
            result = -1;
    }
    else if (strcmp(type, "response.refusal.delta") == 0)
    {
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(event, "delta");
        if (!cJSON_IsString(delta) ||
            close_thinking_block(parser) != 0 ||
            append_text(&parser->response->content,
                        cJSON_GetStringValue(delta)) != 0)
            result = -1;
        else if (parser->on_chunk)
            parser->on_chunk(cJSON_GetStringValue(delta), parser->userdata);
    }
    else if (strcmp(type, "response.refusal.done") == 0)
    {
        cJSON *refusal = cJSON_GetObjectItemCaseSensitive(event, "refusal");
        if (refusal && !cJSON_IsString(refusal)) result = -1;
    }
    else if (strcmp(type, "response.failed") == 0 ||
             strcmp(type, "response.incomplete") == 0 ||
             strcmp(type, "error") == 0)
    {
        parser->terminal_seen = 1;
        parser->failed = 1;
    }
    cJSON_Delete(event);
    return result;
}

static int stream_process_event(StreamParser *parser)
{
    if (parser->event_data.len == 0U) return 0;
    int result = 0;
    if (strcmp(parser->event_data.data, "[DONE]") == 0)
        parser->terminal_seen = 1;
    else
        result = parse_stream_event(parser, parser->event_data.data);
    parser->event_data.len = 0U;
    if (parser->event_data.data) parser->event_data.data[0] = '\0';
    return result;
}

static int stream_process_line(StreamParser *parser)
{
    while (parser->line_len > 0U &&
           (parser->line[parser->line_len - 1U] == '\n' ||
            parser->line[parser->line_len - 1U] == '\r'))
        parser->line_len--;
    parser->line[parser->line_len] = '\0';
    if (parser->line_len == 0U) return stream_process_event(parser);
    if (parser->line[0] == ':') return 0;
    if (strncmp(parser->line, "data:", 5U) != 0) return 0;
    const char *data = parser->line + 5U;
    if (*data == ' ') data++;
    if (parser->event_data.len != 0U &&
        buffer_append(&parser->event_data, "\n", 1U) != 0)
        return -1;
    return buffer_append(&parser->event_data, data, strlen(data));
}

static int stream_feed(StreamParser *parser, const void *bytes, size_t length)
{
    const char *input = bytes;
    if (!parser || (!input && length != 0U)) return -1;
    if (length > OPENAI_MAX_RESPONSE_BYTES ||
        parser->received_bytes > OPENAI_MAX_RESPONSE_BYTES - length)
        return -1;
    parser->received_bytes += length;
    for (size_t i = 0; i < length; i++)
    {
        if (input[i] == '\0' || parser->line_len >= OPENAI_MAX_SSE_EVENT_BYTES)
            return -1;
        if (parser->line_len + 2U > parser->line_cap)
        {
            size_t capacity = parser->line_cap ? parser->line_cap * 2U : 512U;
            if (capacity > OPENAI_MAX_SSE_EVENT_BYTES + 1U)
                capacity = OPENAI_MAX_SSE_EVENT_BYTES + 1U;
            if (capacity < parser->line_len + 2U) return -1;
            char *grown = realloc(parser->line, capacity);
            if (!grown) return -1;
            parser->line = grown;
            parser->line_cap = capacity;
        }
        parser->line[parser->line_len++] = input[i];
        if (input[i] == '\n')
        {
            if (stream_process_line(parser) != 0) return -1;
            parser->line_len = 0U;
        }
    }
    return 0;
}

static int stream_calls_complete(const StreamParser *parser)
{
    for (int i = 0; i < parser->response->tool_calls_count; i++)
    {
        ToolCall *call = &parser->response->tool_calls[i];
        if (!call->id || !call->id[0] || !call->name || !call->name[0] ||
            !call->arguments)
            return 0;
    }
    return 1;
}

static int stream_finish(StreamParser *parser)
{
    if (parser->line_len != 0U)
    {
        if (stream_process_line(parser) != 0) return -1;
        parser->line_len = 0U;
    }
    if (parser->event_data.len != 0U && stream_process_event(parser) != 0)
        return -1;
    if (!parser->terminal_seen || !parser->completed || parser->failed ||
        !stream_calls_complete(parser))
        return -1;
    /* A stream that ended with the think block still open (tool-only turn,
     * truncated summary) must close the tag so the saved message parses. */
    if (close_thinking_block(parser) != 0) return -1;
    if (!parser->response->content &&
        append_text(&parser->response->content, "") != 0)
        return -1;
    return 0;
}

static void stream_parser_cleanup(StreamParser *parser)
{
    if (!parser) return;
    for (size_t i = 0; i < parser->calls_count; i++)
        free(parser->calls[i].item_id);
    free(parser->calls);
    free(parser->line);
    free(parser->event_data.data);
    parser->calls = NULL;
    parser->line = NULL;
    parser->event_data.data = NULL;
}

static size_t header_write_cb(void *ptr, size_t size, size_t nmemb,
                              void *userdata)
{
    LiveContext *context = userdata;
    if (size != 0U && nmemb > SIZE_MAX / size) return 0;
    size_t total = size * nmemb;
    const char *bytes = ptr;
    if (total >= 12U && total < 64U && memcmp(bytes, "HTTP/", 5U) == 0)
    {
        char line[64] = {0};
        memcpy(line, bytes, total);
        char *space = strchr(line, ' ');
        if (!space) return 0;
        char *end = NULL;
        long status = strtol(space + 1, &end, 10);
        if (end == space + 1 || status < 100L || status > 599L) return 0;
        context->status = status;
    }
    return total;
}

static size_t live_write_cb(void *ptr, size_t size, size_t nmemb,
                            void *userdata)
{
    LiveContext *context = userdata;
    if (size != 0U && nmemb > SIZE_MAX / size) return 0;
    size_t total = size * nmemb;
    if (context->status < 200L || context->status >= 300L)
        return buffer_append(&context->error_body, ptr, total) == 0 ? total : 0;
    return stream_feed(&context->parser, ptr, total) == 0 ? total : 0;
}

static LLMResponse *perform_stream(OpenAIOAuth *auth, const char *body,
                                  int timeout,
                                  void (*on_chunk)(const char *, void *),
                                  void *userdata)
{
    Credentials credentials = {0};
    if (credentials_get(auth, &credentials) != 0) return NULL;
    for (int attempt = 0; attempt < 2; attempt++)
    {
        CURL *curl = curl_easy_init();
        struct curl_slist *headers = NULL;
        LLMResponse *response = llm_response_create();
        LiveContext live = {.curl = curl,
                            .error_body = {.limit = OPENAI_MAX_RESPONSE_BYTES}};
        live.parser.response = response;
        live.parser.event_data.limit = OPENAI_MAX_SSE_EVENT_BYTES;
        live.parser.on_chunk = on_chunk;
        live.parser.userdata = userdata;
        CURLcode performed = CURLE_FAILED_INIT;
        if (curl && response &&
            request_setup(curl, body, timeout, &credentials, &headers) == 0 &&
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_write_cb) == CURLE_OK &&
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &live) == CURLE_OK &&
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, live_write_cb) == CURLE_OK &&
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &live) == CURLE_OK)
            performed = curl_easy_perform(curl);
        long status = live.status;
        if (performed == CURLE_OK &&
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK)
            performed = CURLE_GOT_NOTHING;
        headers_free(headers);
        if (curl) curl_easy_cleanup(curl);
        int retry = performed == CURLE_OK && status == 401L && attempt == 0;
        if (retry && credentials_refresh_401(auth, &credentials) == 0)
        {
            stream_parser_cleanup(&live.parser);
            free(live.error_body.data);
            llm_response_free(response);
            continue;
        }
        if (performed != CURLE_OK)
            log_error("OpenAI Codex request failed", "operation", "stream",
                      "error", curl_easy_strerror(performed), NULL);
        else if (status < 200L || status >= 300L)
            log_http_error("stream", status, live.error_body.data);
        else if (stream_finish(&live.parser) != 0)
            log_error("OpenAI Codex stream validation failed", "operation",
                      "stream", NULL);
        else
        {
            stream_parser_cleanup(&live.parser);
            free(live.error_body.data);
            credentials_clear(&credentials);
            return response;
        }
        stream_parser_cleanup(&live.parser);
        free(live.error_body.data);
        llm_response_free(response);
        break;
    }
    credentials_clear(&credentials);
    return NULL;
}

static LLMResponse *openai_chat(LLMProvider *provider, Message *messages,
                                int count, const char *model,
                                double temperature, int timeout,
                                const char *tools_json)
{
    if (!provider || !provider->ctx || timeout <= 0) return NULL;
    OpenAICtx *context = provider->ctx;
    char *body = build_request_body(messages, count, model, temperature, 1,
                                    tools_json, NULL, context->effort);
    if (!body)
    {
        log_error("OpenAI Codex request conversion failed", "operation",
                  "buffered", NULL);
        return NULL;
    }
    LLMResponse *response = perform_stream(context->auth, body, timeout, NULL, NULL);
    free(body);
    return response;
}

static LLMResponse *openai_stream(LLMProvider *provider, Message *messages,
                                  int count, const char *model,
                                  double temperature, int timeout,
                                  void (*on_chunk)(const char *, void *),
                                  void *userdata, const char *tools_json)
{
    if (!provider || !provider->ctx || timeout <= 0) return NULL;
    OpenAICtx *context = provider->ctx;
    char *body = build_request_body(messages, count, model, temperature, 1,
                                    tools_json, NULL, context->effort);
    if (!body)
    {
        log_error("OpenAI Codex request conversion failed", "operation",
                  "stream", NULL);
        return NULL;
    }
    LLMResponse *response = perform_stream(context->auth, body, timeout,
                                           on_chunk, userdata);
    free(body);
    return response;
}

static LLMResponse *openai_structured(LLMProvider *provider, Message *messages,
                                      int count, const char *model,
                                      double temperature, int timeout,
                                      const char *json_schema)
{
    if (!provider || !provider->ctx || timeout <= 0 || !json_schema ||
        !json_schema[0])
        return NULL;
    OpenAICtx *context = provider->ctx;
    char *body = build_request_body(messages, count, model, temperature, 1,
                                    NULL, json_schema, context->effort);
    if (!body)
    {
        log_error("OpenAI Codex request conversion failed", "operation",
                  "structured", NULL);
        return NULL;
    }
    LLMResponse *response = perform_stream(context->auth, body, timeout, NULL, NULL);
    free(body);
    return response;
}

static void openai_destroy(LLMProvider *provider)
{
    if (!provider) return;
    OpenAICtx *context = provider->ctx;
    if (context) free(context->effort);
    free(context);
    free(provider);
}

LLMProvider *openai_provider_create(const char *base_url, const char *api_token,
                                    const char *effort, OpenAIOAuth *auth)
{
    (void)base_url;
    (void)api_token;
    if (!auth) return NULL;
    if (effort && !openai_reasoning_effort_valid(effort))
    {
        log_error("OpenAI provider rejected invalid reasoning effort",
                  "effort", effort ? effort : "", NULL);
        return NULL;
    }
    LLMProvider *provider = calloc(1, sizeof(*provider));
    OpenAICtx *context = calloc(1, sizeof(*context));
    char *effort_copy = effort && effort[0] ? str_dup(effort) : NULL;
    if (!provider || !context || (effort && effort[0] && !effort_copy))
    {
        free(provider);
        free(context);
        free(effort_copy);
        return NULL;
    }
    context->auth = auth;
    context->effort = effort_copy;
    provider->chat = openai_chat;
    provider->chat_streaming = openai_stream;
    provider->extract_structured = openai_structured;
    provider->destroy = openai_destroy;
    provider->ctx = context;
    return provider;
}

#ifdef OPENAI_TEST
char *openai_test_build_request_body(Message *messages, int count,
                                     const char *model, double temperature,
                                     int stream, const char *tools_json,
                                     const char *json_schema,
                                     const char *effort)
{
    return build_request_body(messages, count, model, temperature, stream,
                              tools_json, json_schema, effort);
}

LLMResponse *openai_test_parse_response(const char *raw)
{
    return parse_response(raw);
}

LLMResponse *openai_test_stream_fragments(
    const char **fragments, const size_t *lengths, int count,
    void (*on_chunk)(const char *, void *), void *userdata)
{
    if (!fragments || count < 0) return NULL;
    LLMResponse *response = llm_response_create();
    if (!response) return NULL;
    StreamParser parser = {.response = response,
                           .event_data = {.limit = OPENAI_MAX_SSE_EVENT_BYTES},
                           .on_chunk = on_chunk,
                           .userdata = userdata};
    for (int i = 0; i < count; i++)
    {
        if (!fragments[i] ||
            stream_feed(&parser, fragments[i],
                        lengths ? lengths[i] : strlen(fragments[i])) != 0)
            goto fail;
    }
    if (stream_finish(&parser) != 0) goto fail;
    stream_parser_cleanup(&parser);
    return response;

fail:
    stream_parser_cleanup(&parser);
    llm_response_free(response);
    return NULL;
}

int openai_test_request_metadata(const char *token, const char *account,
                                 const char *body, int timeout,
                                 char **url_out, char **headers_out,
                                 long *timeout_out)
{
    if (!url_out || !headers_out || !timeout_out) return -1;
    *url_out = NULL;
    *headers_out = NULL;
    *timeout_out = 0L;
    CURL *curl = curl_easy_init();
    Credentials credentials = {.token = (char *)token,
                               .account = (char *)account};
    struct curl_slist *headers = NULL;
    if (!curl || request_setup(curl, body, timeout, &credentials, &headers) != 0)
    {
        if (curl) curl_easy_cleanup(curl);
        return -1;
    }
    Buffer joined = {.limit = OPENAI_MAX_TOKEN_BYTES + OPENAI_MAX_ACCOUNT_BYTES + 256U};
    for (struct curl_slist *header = headers; header; header = header->next)
        if (buffer_append(&joined, header->data, strlen(header->data)) != 0 ||
            buffer_append(&joined, "\n", 1U) != 0)
        {
            free(joined.data);
            headers_free(headers);
            curl_easy_cleanup(curl);
            return -1;
        }
    *url_out = str_dup(CODEX_ENDPOINT);
    if (!*url_out)
    {
        free(joined.data);
        headers_free(headers);
        curl_easy_cleanup(curl);
        return -1;
    }
    *headers_out = joined.data;
    *timeout_out = (long)timeout;
    headers_free(headers);
    curl_easy_cleanup(curl);
    return 0;
}

int openai_test_refresh_after_401(OpenAIOAuth *auth,
                                  const char *rejected_token,
                                  char **access_token, char **account_id)
{
    if (!rejected_token || !access_token || !account_id) return -1;
    *access_token = NULL;
    *account_id = NULL;
    Credentials credentials = {.token = str_dup(rejected_token)};
    if (!credentials.token || credentials_refresh_401(auth, &credentials) != 0)
    {
        credentials_clear(&credentials);
        return -1;
    }
    *access_token = credentials.token;
    *account_id = credentials.account;
    credentials.token = NULL;
    credentials.account = NULL;
    return 0;
}

int openai_test_parse_models(const char *raw, char ***models_out,
                             size_t *count_out)
{
    return parse_models_response(raw, models_out, count_out);
}
#endif
