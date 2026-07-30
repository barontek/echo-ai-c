#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "message.h"
#include "../utils/string_utils.h"

Message *message_create(const char *role, const char *content)
{
    Message *msg = calloc(1, sizeof(Message));
    if (!msg) return NULL;
    msg->role = str_dup(role);
    msg->content = str_dup(content ? content : "");
    if (!msg->role) { message_free(msg); return NULL; }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    msg->timestamp = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    return msg;
}

void message_set_tool_calls(Message *msg, ToolCall *calls, int count)
{
    if (msg->tool_calls)
    {
        for (int i = 0; i < msg->tool_calls_count; i++)
            tool_call_free(&msg->tool_calls[i]);
        free(msg->tool_calls);
    }
    msg->tool_calls = calls;
    msg->tool_calls_count = count;
}

int message_copy(Message *dst, const Message *src)
{
    if (!dst || !src) return -1;
    memset(dst, 0, sizeof(*dst));

    dst->role = str_dup(src->role);
    dst->content = str_dup(src->content);
    dst->id = str_dup(src->id);
    dst->tool_call_id = str_dup(src->tool_call_id);
    dst->tool_name = str_dup(src->tool_name);
    dst->error_category = str_dup(src->error_category);
    dst->thinking = str_dup(src->thinking);
    dst->timestamp = src->timestamp;

    if ((src->role && !dst->role) || (src->content && !dst->content) ||
        (src->id && !dst->id) || (src->tool_call_id && !dst->tool_call_id) ||
        (src->tool_name && !dst->tool_name) ||
        (src->error_category && !dst->error_category) ||
        (src->thinking && !dst->thinking))
        goto cleanup;

    if (src->tool_calls && src->tool_calls_count > 0)
    {
        dst->tool_calls = calloc((size_t)src->tool_calls_count, sizeof(ToolCall));
        if (!dst->tool_calls) goto cleanup;
        dst->tool_calls_count = src->tool_calls_count;
        for (int i = 0; i < src->tool_calls_count; i++)
        {
            dst->tool_calls[i].id = str_dup(src->tool_calls[i].id);
            dst->tool_calls[i].name = str_dup(src->tool_calls[i].name);
            dst->tool_calls[i].arguments = str_dup(src->tool_calls[i].arguments);
            dst->tool_calls[i].result_content = str_dup(src->tool_calls[i].result_content);
            dst->tool_calls[i].result_error = str_dup(src->tool_calls[i].result_error);
            if ((src->tool_calls[i].id && !dst->tool_calls[i].id) ||
                (src->tool_calls[i].name && !dst->tool_calls[i].name) ||
                (src->tool_calls[i].arguments && !dst->tool_calls[i].arguments) ||
                (src->tool_calls[i].result_content && !dst->tool_calls[i].result_content) ||
                (src->tool_calls[i].result_error && !dst->tool_calls[i].result_error))
                goto cleanup;
        }
    }

    return 0;

cleanup:
    /* every field is either a valid copy or NULL (calloc/memset), so this is safe */
    free(dst->role);
    free(dst->content);
    free(dst->id);
    free(dst->tool_call_id);
    free(dst->tool_name);
    free(dst->error_category);
    free(dst->thinking);
    if (dst->tool_calls)
    {
        for (int i = 0; i < dst->tool_calls_count; i++)
            tool_call_free(&dst->tool_calls[i]);
        free(dst->tool_calls);
    }
    memset(dst, 0, sizeof(*dst));
    return -1;
}

void message_free(Message *msg)
{
    if (!msg) return;
    message_clear(msg);
    free(msg);
}

void message_clear(Message *msg)
{
    if (!msg) return;
    free(msg->role);
    free(msg->content);
    free(msg->id);
    free(msg->tool_call_id);
    free(msg->tool_name);
    free(msg->error_category);
    free(msg->thinking);
    if (msg->tool_calls)
    {
        for (int i = 0; i < msg->tool_calls_count; i++)
            tool_call_free(&msg->tool_calls[i]);
        free(msg->tool_calls);
    }
    /* Zero the owned pointers and count so a stale reference can't double-free
     * or be used after clear. */
    msg->role = NULL;
    msg->content = NULL;
    msg->id = NULL;
    msg->tool_call_id = NULL;
    msg->tool_name = NULL;
    msg->error_category = NULL;
    msg->thinking = NULL;
    msg->tool_calls = NULL;
    msg->tool_calls_count = 0;
}

void message_free_all(Message *msgs, int count)
{
    for (int i = 0; i < count; i++)
        message_clear(&msgs[i]);
    free(msgs);
}

ToolCall *tool_call_create(const char *id, const char *name, const char *arguments)
{
    ToolCall *tc = calloc(1, sizeof(ToolCall));
    if (!tc) return NULL;
    tc->id = str_dup(id);
    tc->name = str_dup(name);
    tc->arguments = str_dup(arguments);
    if (!tc->id || !tc->name || !tc->arguments)
    {
        tool_call_free(tc);
        free(tc);
        return NULL;
    }
    return tc;
}

void tool_call_free(ToolCall *call)
{
    if (!call) return;
    free(call->id);
    free(call->name);
    free(call->arguments);
    free(call->result_content);
    free(call->result_error);
}

LLMResponse *llm_response_create(void)
{
    return calloc(1, sizeof(LLMResponse));
}

void llm_response_free(LLMResponse *resp)
{
    if (!resp) return;
    free(resp->content);
    free(resp->thinking);
    if (resp->tool_calls)
    {
        for (int i = 0; i < resp->tool_calls_count; i++)
            tool_call_free(&resp->tool_calls[i]);
        free(resp->tool_calls);
    }
    free(resp);
}

cJSON *messages_to_json_array(Message *msgs, int count)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_CreateObject();
        if (!item) { cJSON_Delete(arr); return NULL; }

        cJSON_AddStringToObject(item, "role", msgs[i].role ? msgs[i].role : "");
        cJSON_AddStringToObject(item, "content", msgs[i].content ? msgs[i].content : "");

        if (msgs[i].thinking)
            cJSON_AddStringToObject(item, "thinking", msgs[i].thinking);

        if (msgs[i].tool_name)
            cJSON_AddStringToObject(item, "tool_name", msgs[i].tool_name);

        if (msgs[i].error_category)
            cJSON_AddStringToObject(item, "error_category", msgs[i].error_category);

        /* Persist per-message id so reload preserves stable identity. Emit
         * only when non-NULL to keep legacy blobs shape-equivalent on first
         * save. The deserializer reads it back into Message.id. */
        if (msgs[i].id)
            cJSON_AddStringToObject(item, "id", msgs[i].id);

        /* Persist timestamp so reload preserves per-message timing. Emit only
         * when non-zero to keep legacy blobs (which never carried a timestamp)
         * shape-equivalent on first save. */
        if (msgs[i].timestamp > 0.0)
            cJSON_AddNumberToObject(item, "timestamp", msgs[i].timestamp);

        if (msgs[i].tool_calls && msgs[i].tool_calls_count > 0)
        {
            cJSON *tc_arr = cJSON_CreateArray();
            for (int j = 0; j < msgs[i].tool_calls_count; j++)
            {
                cJSON *tc = cJSON_CreateObject();
                cJSON_AddStringToObject(tc, "id", msgs[i].tool_calls[j].id ? msgs[i].tool_calls[j].id : "");
                cJSON_AddStringToObject(tc, "type", "function");
                cJSON *fn = cJSON_CreateObject();
                cJSON_AddStringToObject(fn, "name", msgs[i].tool_calls[j].name ? msgs[i].tool_calls[j].name : "");

                cJSON *args = cJSON_Parse(msgs[i].tool_calls[j].arguments);
                if (args)
                    cJSON_AddItemToObject(fn, "arguments", args);
                else
                    cJSON_AddStringToObject(fn, "arguments", msgs[i].tool_calls[j].arguments ? msgs[i].tool_calls[j].arguments : "");

                cJSON_AddItemToObject(tc, "function", fn);

                /* Persist tool results so reload preserves UI state. Emit only
                 * when non-NULL to keep legacy blobs shape-equivalent on first
                 * save; the deserializer reads them back. */
                if (msgs[i].tool_calls[j].result_content)
                    cJSON_AddStringToObject(tc, "result_content", msgs[i].tool_calls[j].result_content);
                if (msgs[i].tool_calls[j].result_error)
                    cJSON_AddStringToObject(tc, "result_error", msgs[i].tool_calls[j].result_error);

                cJSON_AddItemToArray(tc_arr, tc);
            }
            cJSON_AddItemToObject(item, "tool_calls", tc_arr);
        }

        if (msgs[i].tool_call_id)
            cJSON_AddStringToObject(item, "tool_call_id", msgs[i].tool_call_id);

        cJSON_AddItemToArray(arr, item);
    }

    return arr;
}

char *llm_messages_format(Message *msgs, int count,
                          const char *system_prompt,
                          const char *system_context)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    if (system_prompt)
    {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        char *full_prompt = NULL;
        if (system_context)
        {
            if (asprintf(&full_prompt, "%s\n\n%s", system_prompt, system_context) < 0)
                full_prompt = NULL;
        }
        else
        {
            full_prompt = str_dup(system_prompt);
        }
        cJSON_AddStringToObject(sys, "content", full_prompt ? full_prompt : system_prompt);
        free(full_prompt);
        cJSON_AddItemToArray(arr, sys);
    }

    cJSON *msgs_json = messages_to_json_array(msgs, count);
    if (msgs_json)
    {
        int len = cJSON_GetArraySize(msgs_json);
        for (int i = 0; i < len; i++)
        {
            cJSON *item = cJSON_DetachItemFromArray(msgs_json, 0);
            if (item) cJSON_AddItemToArray(arr, item);
        }
        cJSON_Delete(msgs_json);
    }

    char *result = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return result;
}
