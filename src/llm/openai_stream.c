/*
 * openai_stream.c - Codex SSE stream parser: event dispatch,
 * function-call delta folding, thinking-block emission, and
 * terminal validation.
 * Depends on: cJSON, http_client, string_utils, openai_response.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "openai.h"
#include "openai_internal.h"
#include "openai_stream.h"
#include "openai_request.h"
#include "openai_response.h"
#include "../utils/http_client.h"
#include "../utils/string_utils.h"


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
                : str_append(&call->arguments, value);
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
         {
            cJSON_Delete(reasoning);
            return -1;
        }
        if (strcmp(cJSON_GetStringValue(type), "reasoning") == 0)
        {
            cJSON *copy = cJSON_Duplicate(item, 1);
            if (!copy || !cJSON_AddItemToArray(reasoning, copy))
             {
                cJSON_Delete(copy);
                cJSON_Delete(reasoning);
                return -1;
            }
        }
        else if (strcmp(cJSON_GetStringValue(type), "function_call") == 0 &&
            upsert_function_item(parser, index, item) != 0)
         {  // NOLINT(bugprone-branch-clone): shared delete+return error tail;
            // TODO(openai_stream): the message branch below is intentional
            // (each branch has a distinct side effect, only the error tail clones)
            cJSON_Delete(reasoning);
            return -1;
        }
        else if (strcmp(cJSON_GetStringValue(type), "message") == 0 &&
                 capture_message_phase(parser->response, item) != 0)
         {
            cJSON_Delete(reasoning);
            return -1;
        }
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

static int emit_thinking_text(StreamParser *parser, const char *text)
{
    if (!text || !text[0]) return 0;
    if (!parser->thinking_open)
    {
        if (str_append(&parser->response->content, "<think>\n") != 0) return -1;
        if (parser->on_chunk) parser->on_chunk("<think>\n", parser->userdata);
        parser->thinking_open = 1;
    }
    if (str_append(&parser->response->content, text) != 0) return -1;
    if (parser->on_chunk) parser->on_chunk(text, parser->userdata);
    return 0;
}

static int close_thinking_block(StreamParser *parser)
{
    if (!parser->thinking_open) return 0;
    if (str_append(&parser->response->content, "\n</think>\n\n") != 0) return -1;
    if (parser->on_chunk) parser->on_chunk("\n</think>\n\n", parser->userdata);
    parser->thinking_open = 0;
    return 0;
}

char *reasoning_summary_join(const cJSON *item)
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
            if (str_append(&joined, "\n") != 0 ||
                str_append(&joined, part) != 0)
             {
                free(joined);
                return NULL;
            }
        }
    }
    return joined;
}

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
        if (strncmp(src, "<!--", 4) == 0) {
            src += 4;
            continue;
        }
        if (strncmp(src, "-->", 3) == 0) {
            src += 3;
            continue;
        }
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
    if (!valid_nonempty_string(type_json)) {
        cJSON_Delete(event);
        return -1;
    }
    const char *type = cJSON_GetStringValue(type_json);
    int result = 0;
    if (strcmp(type, "response.output_text.delta") == 0)
    {
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(event, "delta");
        if (!cJSON_IsString(delta) ||
            close_thinking_block(parser) != 0 ||
            str_append(&parser->response->content,
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
            str_append(&parser->response->content,
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
        http_buffer_append(&parser->event_data, "\n", 1U) != 0)
        return -1;
    return http_buffer_append(&parser->event_data, data, strlen(data));
}

int stream_feed(StreamParser *parser, const void *bytes, size_t length)
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

int stream_finish(StreamParser *parser)
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
        str_append(&parser->response->content, "") != 0)
        return -1;
    return 0;
}

void stream_parser_cleanup(StreamParser *parser)
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
