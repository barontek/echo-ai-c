/*
 * openai_request.c - Codex request-body builder: message
 * serialization, tool conversion, structured-output schema,
 * and reasoning-effort configuration.
 * Depends on: cJSON, string_utils, message types.
 */

#define _GNU_SOURCE
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "openai.h"
#include "openai_internal.h"
#include "openai_request.h"
#include "../utils/string_utils.h"


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

int valid_nonempty_string(const cJSON *item)
{
    const char *value = cJSON_IsString(item) ? cJSON_GetStringValue(item) : NULL;
    return value && value[0] != '\0';
}

int parse_bounded_json(const char *text, size_t limit, cJSON **json_out)
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
    if (*instructions && (*instructions)[0] != '\0' && str_append(instructions, "\n") != 0)
        return -1;
    return str_append(instructions, text);
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
         {
            cJSON_Delete(items);
            return -1;
        }
        cJSON *copy = cJSON_Duplicate(item, 1);
        if (!copy)
         {
            cJSON_Delete(items);
            return -1;
        }
        if (json_array_add(input, copy) != 0)
         {
            cJSON_Delete(items);
            return -1;
        }
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
    if (!result) {
        cJSON_Delete(source);
        return NULL;
    }
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

char *build_request_body(Message *messages, int count, const char *model,
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
        else if (json_add_item(root, "tools", tools) != 0 ||
                 json_add_bool(root, "parallel_tool_calls", 1) != 0)
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
