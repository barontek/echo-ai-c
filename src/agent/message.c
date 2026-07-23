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

void message_free(Message *msg)
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
    free(msg);
}

void message_free_all(Message *msgs, int count)
{
    for (int i = 0; i < count; i++)
    {
        free(msgs[i].role);
        free(msgs[i].content);
        free(msgs[i].id);
        free(msgs[i].tool_call_id);
        free(msgs[i].tool_name);
        free(msgs[i].error_category);
        free(msgs[i].thinking);
        if (msgs[i].tool_calls)
        {
            for (int j = 0; j < msgs[i].tool_calls_count; j++)
                tool_call_free(&msgs[i].tool_calls[j]);
            free(msgs[i].tool_calls);
        }
    }
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

        if (msgs[i].tool_calls_count > 0)
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
            cJSON *item = cJSON_DetachItemFromArray(msgs_json, i);
            cJSON_AddItemToArray(arr, item);
        }
        cJSON_Delete(msgs_json);
    }

    char *result = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return result;
}
