/*
 * openai_compatible_stream.c - SSE stream parsing for the
 * OpenAI-compatible client: line framing, event dispatch, tool-call
 * delta folding, and thinking-block emission.
 * Depends on: cJSON, string_utils.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <cjson/cJSON.h>

#include "openai_compatible_stream.h"
#include "openai_compatible.h"
#include "../utils/http_client.h"
#include "../utils/string_utils.h"



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

static int forward_reasoning_delta(LLMResponse *resp, int *thinking_open,
                                   const char *text,
                                   void (*on_chunk)(const char *, void *),
                                   void *userdata)
{
    if (!text || !text[0]) return 0;
    if (!*thinking_open)
    {
        if (str_append(&resp->content, "<think>\n") != 0) return -1;
        if (on_chunk) on_chunk("<think>\n", userdata);
        *thinking_open = 1;
    }
    if (str_append(&resp->content, text) != 0) return -1;
    if (on_chunk) on_chunk(text, userdata);
    return 0;
}

static int parse_stream_event(LLMResponse *resp, const char *json_text,
                              int *thinking_open,
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

    /* Reasoning deltas: DeepSeek/Qwen/GLM send delta.reasoning_content,
     * Kimi (and some others) send delta.reasoning. Both are plain text of
     * what the model thought before answering. */
    cJSON *reasoning = cJSON_GetObjectItem(delta, "reasoning_content");
    if (!reasoning) reasoning = cJSON_GetObjectItem(delta, "reasoning");
    if (reasoning && cJSON_IsString(reasoning) &&
        forward_reasoning_delta(resp, thinking_open,
                                cJSON_GetStringValue(reasoning),
                                on_chunk, userdata) != 0)
    {
        cJSON_Delete(json);
        return -1;
    }

    cJSON *content = cJSON_GetObjectItem(delta, "content");
    if (content && cJSON_IsString(content) &&
        cJSON_GetStringValue(content)[0] != '\0')
    {
        const char *chunk = cJSON_GetStringValue(content);
        if (*thinking_open)
        {
            if (str_append(&resp->content, "\n</think>\n\n") != 0)
            {
                cJSON_Delete(json);
                return -1;
            }
            if (on_chunk) on_chunk("\n</think>\n\n", userdata);
            *thinking_open = 0;
        }
        if (str_append(&resp->content, chunk) != 0)
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
                 str_append(&call->id, cJSON_GetStringValue(id)) != 0) ||
                (name && cJSON_IsString(name) &&
                 str_append(&call->name, cJSON_GetStringValue(name)) != 0) ||
                (arguments && cJSON_IsString(arguments) &&
                 str_append(&call->arguments, cJSON_GetStringValue(arguments)) != 0))
            {
                cJSON_Delete(json);
                return -1;
            }
        }
    }

    cJSON_Delete(json);
    return 0;
}

int stream_parser_feed(StreamParser *p, const char *bytes, size_t len)
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
                                   &p->thinking_open,
                                   p->on_chunk, p->userdata) != 0)
                return -1;
        }
    }
    return 0;
}

int stream_parser_finish(StreamParser *p)
{
    if (p->line_len != 0)
    {
        /* final partial line without a trailing newline */
        if (p->line[p->line_len - 1] == '\r')
            p->line[--p->line_len] = '\0';
        else
            p->line[p->line_len] = '\0';
        p->line_len = 0;
        if (strncmp(p->line, "data: ", 6) == 0 &&
            strcmp(p->line + 6, "[DONE]") != 0)
        {
            if (parse_stream_event(p->resp, p->line + 6,
                                   &p->thinking_open,
                                   p->on_chunk, p->userdata) != 0)
                return -1;
        }
    }
    /* a stream that ended while reasoning was still open (no content ever
     * arrived) must still close the tag so the saved message parses */
    if (p->thinking_open)
    {
        if (str_append(&p->resp->content, "\n</think>\n\n") != 0) return -1;
        if (p->on_chunk) p->on_chunk("\n</think>\n\n", p->userdata);
        p->thinking_open = 0;
    }
    return 0;
}

#ifdef OPENAI_COMPATIBLE_TEST
LLMResponse *openai_compatible_test_parse_stream_alloc(
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
        if (!resp->content) {
            llm_response_free(resp);
            return NULL;
        }
    }
    return resp;
}

LLMResponse *openai_compatible_test_stream_fragments_alloc(
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
        if (!resp->content) {
            llm_response_free(resp);
            return NULL;
        }
    }
    return resp;
}
#endif
