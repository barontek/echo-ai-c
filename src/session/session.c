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
            message_clear(&s->messages[i]);
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

/* Parse `json_str` as a JSON array of messages and replace `session->messages`
 * with the result. If `session->messages` is already non-NULL, the prior
 * array and all owned strings are freed first (consistent with
 * session_deserialize_metadata/_events). On parse failure `session->messages`
 * is left unchanged. Returns 0 on success, -1 on parse or alloc failure.
 * Owns: the caller owns `session` and all memory within it after return. */
int session_deserialize_messages(Session *session, const char *json_str)
{
    if (!json_str) return -1;
    cJSON *arr = cJSON_Parse(json_str);
    if (!arr || !cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); return -1; }

    int count = cJSON_GetArraySize(arr);
    if (session->messages)
    {
        message_free_all(session->messages, session->messages_count);
        session->messages = NULL;
        session->messages_count = 0;
    }
    session->messages = calloc((size_t)count, sizeof(Message));
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
        cJSON *ts_item = cJSON_GetObjectItem(item, "timestamp");

        session->messages[i].role = str_dup(role && role->valuestring ? role->valuestring : "");
        session->messages[i].content = str_dup(content && content->valuestring ? content->valuestring : "");
        session->messages[i].id = str_dup(msg_id && msg_id->valuestring ? msg_id->valuestring : NULL);
        session->messages[i].tool_call_id = str_dup(tool_call_id && tool_call_id->valuestring ? tool_call_id->valuestring : NULL);
        session->messages[i].tool_name = str_dup(tool_name && tool_name->valuestring ? tool_name->valuestring : NULL);
        session->messages[i].error_category = str_dup(error_cat && error_cat->valuestring ? error_cat->valuestring : NULL);
        session->messages[i].thinking = str_dup(thinking && thinking->valuestring ? thinking->valuestring : NULL);
        session->messages[i].timestamp = (ts_item && cJSON_IsNumber(ts_item)) ? ts_item->valuedouble : 0.0;

        if (tool_calls_arr && cJSON_IsArray(tool_calls_arr))
        {
            int tc_count = cJSON_GetArraySize(tool_calls_arr);
            session->messages[i].tool_calls = calloc((size_t)tc_count, sizeof(ToolCall));
            if (session->messages[i].tool_calls)
            {
                session->messages[i].tool_calls_count = tc_count;
                for (int j = 0; j < tc_count; j++)
                {
                    cJSON *tc = cJSON_GetArrayItem(tool_calls_arr, j);
                    if (!tc) continue;
                    cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
                    cJSON *tc_fn = cJSON_GetObjectItem(tc, "function");
                    cJSON *tc_name = tc_fn ? cJSON_GetObjectItem(tc_fn, "name") : NULL;
                    cJSON *tc_args = tc_fn ? cJSON_GetObjectItem(tc_fn, "arguments") : NULL;
                    cJSON *tc_rc = cJSON_GetObjectItem(tc, "result_content");
                    cJSON *tc_re = cJSON_GetObjectItem(tc, "result_error");
                    session->messages[i].tool_calls[j].id = str_dup(tc_id && tc_id->valuestring ? tc_id->valuestring : NULL);
                    session->messages[i].tool_calls[j].name = str_dup(tc_name && tc_name->valuestring ? tc_name->valuestring : NULL);
                    if (tc_args)
                    {
                        char *arg_str = cJSON_PrintUnformatted(tc_args);
                        session->messages[i].tool_calls[j].arguments = arg_str;
                    }
                    session->messages[i].tool_calls[j].result_content = str_dup(tc_rc && tc_rc->valuestring ? tc_rc->valuestring : NULL);
                    session->messages[i].tool_calls[j].result_error = str_dup(tc_re && tc_re->valuestring ? tc_re->valuestring : NULL);
                }
            }
        }
    }

    cJSON_Delete(arr);
    return 0;
}

/* C14: serialize/deserialize round-trip COLLAPSES the two distinct
 * "metadata not set" and "metadata explicitly empty {}" combinations into
 * one DB representation. `session_serialize_metadata` returns:
 *   NULL  iff session->metadata == NULL (legitimate "no metadata ever set")
 *   "{}"  iff session->metadata is non-NULL but empty (legitimate "user
 *         explicitly cleared it")
 *   "{ ...real json... }" otherwise
 * `session_manager_save_session` then binds SQL NULL whenever the JSON is
 * NULL — and (per the early-call-path design of load_session) a NULL blob
 * on load means we don't even call deserialize, leaving the freshly-created
 * session->metadata (which is initialized to `cJSON_CreateObject()` — empty
 * but non-NULL) in place. The `{}` case over the wire bind-NULLS as well
 * (because the fix in this file returns a non-NULL "{}" but the save path's
 * metadata_json assignment + bind-null branch only fires when meta_enc is
 * NULL, which only happens under encrypt-OOM or empty-meta-but-non-empty-
 * metadata). End result: both "never set" and "explicitly {}", on next
 * load, present as a non-NULL session->metadata holding an empty object.
 *
 * This is a deliberate, documented collapse — distinguishing the two would
 * require either a separate "explicit empty" sentinel column (schema churn)
 * or accepting an asymmetry where save `{}` -> `metadata_encrypted IS NULL`
 * but save `{"k":1}` -> non-NULL. The chat side never depends on the
 * distinction (treating `session->metadata` as "you may add keys" works for
 * both). No fix beyond this comment; flagging it so a future consumer that
 * tries to depend on the distinction doesn't get surprised.
 *
 * Returns: NULL iff session->metadata is NULL; otherwise caller owns a
 * malloc'd NUL-terminated JSON string (either "{}" or a real object). */
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
