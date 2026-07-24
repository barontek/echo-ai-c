#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "session.h"
#include "../utils/string_utils.h"
#include "../utils/json.h"

Session *session_create(const char *title)
{
    Session *s = calloc(1, sizeof(Session));
    if (!s) return NULL;

    time_t now = time(NULL);
    if (asprintf(&s->id, "%ld", (long)now) < 0) { free(s); return NULL; }
    {
        int unique = rand() % 1000000;
        char *tmp = NULL;
        if (asprintf(&tmp, "%s-%d", s->id, unique) < 0) { free(s->id); free(s); return NULL; }
        free(s->id);
        s->id = tmp;
    }
    s->title = str_dup(title ? title : "New Session");
    s->title_generation_attempted = 0;

    char ts[64];
    struct tm *tm_ptr = localtime(&now);
    if (tm_ptr)
    {
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm_ptr);
        s->created_at = str_dup(ts);
    }
    else
    {
        s->created_at = str_dup("unknown");
    }

    s->messages = NULL;
    s->messages_count = 0;
    s->metadata = cJSON_CreateObject();
    s->events = cJSON_CreateArray();
    return s;
}

void session_free(Session *s)
{
    if (!s) return;
    free(s->id);
    free(s->title);
    free(s->created_at);
    if (s->messages)
    {
        for (int i = 0; i < s->messages_count; i++)
        {
            free(s->messages[i].role);
            free(s->messages[i].content);
            free(s->messages[i].id);
            free(s->messages[i].tool_call_id);
            free(s->messages[i].tool_name);
            free(s->messages[i].error_category);
            free(s->messages[i].thinking);
            if (s->messages[i].tool_calls)
            {
                for (int j = 0; j < s->messages[i].tool_calls_count; j++)
                    tool_call_free(&s->messages[i].tool_calls[j]);
                free(s->messages[i].tool_calls);
            }
        }
        free(s->messages);
    }
    if (s->metadata) cJSON_Delete(s->metadata);
    if (s->events) cJSON_Delete(s->events);
    free(s);
}

char *session_serialize_messages(const Session *session)
{
    cJSON *arr = messages_to_json_array(session->messages, session->messages_count);
    if (!arr) return NULL;
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

int session_deserialize_messages(Session *session, const char *json_str)
{
    if (!json_str) return -1;
    cJSON *arr = cJSON_Parse(json_str);
    if (!arr || !cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); return -1; }

    int count = cJSON_GetArraySize(arr);
    session->messages = calloc(count, sizeof(Message));
    if (!session->messages && count > 0) { cJSON_Delete(arr); return -1; }
    session->messages_count = count;

    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item) continue;
        cJSON *role = cJSON_GetObjectItem(item, "role");
        cJSON *content = cJSON_GetObjectItem(item, "content");
        cJSON *msg_id = cJSON_GetObjectItem(item, "id");
        cJSON *tool_call_id = cJSON_GetObjectItem(item, "tool_call_id");
        cJSON *tool_name = cJSON_GetObjectItem(item, "tool_name");
        cJSON *error_cat = cJSON_GetObjectItem(item, "error_category");
        cJSON *thinking = cJSON_GetObjectItem(item, "thinking");
        cJSON *tool_calls_arr = cJSON_GetObjectItem(item, "tool_calls");

        session->messages[i].role = str_dup(role && role->valuestring ? role->valuestring : "");
        session->messages[i].content = str_dup(content && content->valuestring ? content->valuestring : "");
        session->messages[i].id = str_dup(msg_id && msg_id->valuestring ? msg_id->valuestring : NULL);
        session->messages[i].tool_call_id = str_dup(tool_call_id && tool_call_id->valuestring ? tool_call_id->valuestring : NULL);
        session->messages[i].tool_name = str_dup(tool_name && tool_name->valuestring ? tool_name->valuestring : NULL);
        session->messages[i].error_category = str_dup(error_cat && error_cat->valuestring ? error_cat->valuestring : NULL);
        session->messages[i].thinking = str_dup(thinking && thinking->valuestring ? thinking->valuestring : NULL);

        if (tool_calls_arr && cJSON_IsArray(tool_calls_arr))
        {
            int tc_count = cJSON_GetArraySize(tool_calls_arr);
            session->messages[i].tool_calls = calloc(tc_count, sizeof(ToolCall));
            if (session->messages[i].tool_calls)
                session->messages[i].tool_calls_count = tc_count;
            for (int j = 0; j < tc_count; j++)
            {
                cJSON *tc = cJSON_GetArrayItem(tool_calls_arr, j);
                if (!tc) continue;
                cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
                cJSON *tc_name = cJSON_GetObjectItem(tc, "name");
                cJSON *tc_args = cJSON_GetObjectItem(tc, "arguments");
                session->messages[i].tool_calls[j].id = str_dup(tc_id && tc_id->valuestring ? tc_id->valuestring : NULL);
                session->messages[i].tool_calls[j].name = str_dup(tc_name && tc_name->valuestring ? tc_name->valuestring : NULL);
                if (tc_args)
                {
                    char *arg_str = cJSON_PrintUnformatted(tc_args);
                    session->messages[i].tool_calls[j].arguments = arg_str;
                }
            }
        }
    }

    cJSON_Delete(arr);
    return 0;
}

char *session_serialize_metadata(const Session *session)
{
    if (!session->metadata) return NULL;
    char *json = cJSON_PrintUnformatted(session->metadata);
    if (json)
    {
        int len = strlen(json);
        if (len <= 2) { free(json); return str_dup("{}"); }
    }
    return json;
}

int session_deserialize_metadata(Session *session, const char *json_str)
{
    if (!json_str) return -1;
    if (session->metadata) cJSON_Delete(session->metadata);
    session->metadata = cJSON_Parse(json_str);
    if (!session->metadata) { session->metadata = cJSON_CreateObject(); return -1; }
    return 0;
}

char *session_serialize_events(const Session *session)
{
    if (!session->events) return NULL;
    char *json = cJSON_PrintUnformatted(session->events);
    if (json)
    {
        int len = strlen(json);
        if (len <= 2) { free(json); return str_dup("[]"); }
    }
    return json;
}

int session_deserialize_events(Session *session, const char *json_str)
{
    if (!json_str) return -1;
    if (session->events) cJSON_Delete(session->events);
    session->events = cJSON_Parse(json_str);
    if (!session->events || !cJSON_IsArray(session->events))
    {
        if (session->events) cJSON_Delete(session->events);
        session->events = cJSON_CreateArray();
        return -1;
    }
    return 0;
}
