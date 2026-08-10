/*
 * session.c - in-memory Session lifecycle: create/free, and JSON
 * serialization/deserialization of messages, metadata, and events.
 * Depends on: cJSON, agent/message.h, utils (string_utils, json).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "session.h"
#include "../utils/string_utils.h"
#include "../utils/json.h"

/* Create a new in-memory Session with minted id, default-empty metadata/-
 * events, and a timestamped created_at. Returns a fully-owned Session or
 * NULL on any allocation failure (in which case all partial state is freed
 * before return). Caller owns the returned Session and must free via
 * session_free. */
Session *session_create(const char *title)
{
    Session *s = calloc(1, sizeof(Session));
    if (!s) return NULL;

    time_t now = time(NULL);
    if (asprintf(&s->id, "%ld", (long)now) < 0) { free(s); return NULL; }
    {
        /* The timestamp prefix alone collides for sessions created in the
         * same second; a random suffix makes ids practically unique. */
        int unique = rand() % 1000000;
        char *tmp = NULL;
        if (asprintf(&tmp, "%s-%d", s->id, unique) < 0) { free(s->id); free(s); return NULL; }
        free(s->id);
        s->id = tmp;
    }

    /* F1: str_dup can fail under OOM. Old code assigned the (possibly-NULL)
     * result directly into s->title and let it flow on to sqlite3_bind_text
     * as SQL NULL — a session silently created with no title. Now we abort
     * the create and free all partial state. */
    s->title = str_dup(title ? title : "New Session");
    if (!s->title) { free(s->id); free(s); return NULL; }

    s->title_generation_attempted = 0;

    /* D3: localtime is thread-unsafe (writes to a shared static buffer);
     * under a multi-threaded server two concurrent calls (here and in
     * session_manager purge_sessions/log_event) race and can corrupt the
     * stored created_at. localtime_r is the thread-safe variant. */
    char ts[64];
    struct tm tm_storage;
    struct tm *tm_ptr = localtime_r(&now, &tm_storage);
    if (tm_ptr)
    {
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm_ptr);
        s->created_at = str_dup(ts);
    }
    else
    {
        s->created_at = str_dup("unknown");
    }
    /* F1: created_at str_dup failure also aborts. */
    if (!s->created_at) { free(s->title); free(s->id); free(s); return NULL; }

    s->messages = NULL;
    s->messages_count = 0;

    /* F2: cJSON_CreateObject/CreateArray can return NULL under alloc failure.
     * Old code returned `s` with metadata/events == NULL — inconsistent with
     * the rest of the codebase's "session->metadata/events always non-NULL
     * after session_create" assumption. Now we abort the create. */
    s->metadata = cJSON_CreateObject();
    s->events = cJSON_CreateArray();
    if (!s->metadata || !s->events)
    {
        if (s->metadata) cJSON_Delete(s->metadata);
        if (s->events) cJSON_Delete(s->events);
        free(s->created_at);
        free(s->title);
        free(s->id);
        free(s);
        return NULL;
    }

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

/* F5: serialize `session->messages` as a NUL-terminated JSON array string.
 * `session` must be non-NULL (no NULL-guard — caller responsibility, the
 * single existing caller `session_manager_save_session` always passes a
 * freshly-loaded or freshly-created Session). Returns a malloc'd string the
 * caller must `free`, or NULL on cJSON-print OOM. A successful return is
 * always at least `"[]"` (cJSON_PrintUnformatted of an empty array); the
 * C13 fix in `save_session_core` treats a NULL return here as OOM and
 * refuses the save. */
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
 *
 * C2: every str_dup/calloc failure now propagates through unified cleanup
 * labels. For each field the NULL check distinguishes OOM from missing-key:
 * role and content are always present (guaranteed non-NULL str_dup result
 * means OOM); optional fields (id, parent_id, fork_group_id, tool_call_id,
 * tool_name, error_category, thinking, phase, provider_state, and tool_call
 * inner members) only treat a NULL str_dup as OOM
 * when the source cJSON item was actually a non-NULL string. The cleanup
 * labels route everything through message_clear (NULL-safe, idempotent,
 * zeros the struct) — same pattern as the C15 fix. */
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

    int i;
    for (i = 0; i < count; i++)
    {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item) continue;
        cJSON *role = cJSON_GetObjectItem(item, "role");
        cJSON *content = cJSON_GetObjectItem(item, "content");
        cJSON *msg_id = cJSON_GetObjectItem(item, "id");
        cJSON *parent_id = cJSON_GetObjectItem(item, "parent_id");
        cJSON *fork_group_id = cJSON_GetObjectItem(item, "fork_group_id");
        cJSON *tool_call_id = cJSON_GetObjectItem(item, "tool_call_id");
        cJSON *tool_name = cJSON_GetObjectItem(item, "tool_name");
        cJSON *error_cat = cJSON_GetObjectItem(item, "error_category");
        cJSON *thinking = cJSON_GetObjectItem(item, "thinking");
        cJSON *phase = cJSON_GetObjectItem(item, "phase");
        cJSON *provider_state = cJSON_GetObjectItem(item, "provider_state");
        cJSON *tool_calls_arr = cJSON_GetObjectItem(item, "tool_calls");
        cJSON *ts_item = cJSON_GetObjectItem(item, "timestamp");

        /* role and content are always-present fields — a NULL str_dup here
         * is OOM, not a missing key (the ternary always yields at least ""). */
        session->messages[i].role = str_dup(role && role->valuestring ? role->valuestring : "");
        if (!session->messages[i].role) goto partial_fail_msg;
        session->messages[i].content = str_dup(content && content->valuestring ? content->valuestring : "");
        if (!session->messages[i].content) goto partial_fail_msg;

        /* optional fields: NULL from str_dup is only OOM when source was a real string */
        session->messages[i].id = str_dup(msg_id && msg_id->valuestring ? msg_id->valuestring : NULL);
        if (!session->messages[i].id && msg_id && msg_id->valuestring) goto partial_fail_msg;
        session->messages[i].parent_id = str_dup(parent_id && parent_id->valuestring ? parent_id->valuestring : NULL);
        if (!session->messages[i].parent_id && parent_id && parent_id->valuestring) goto partial_fail_msg;
        session->messages[i].fork_group_id = str_dup(fork_group_id && fork_group_id->valuestring ? fork_group_id->valuestring : NULL);
        if (!session->messages[i].fork_group_id && fork_group_id && fork_group_id->valuestring) goto partial_fail_msg;
        session->messages[i].tool_call_id = str_dup(tool_call_id && tool_call_id->valuestring ? tool_call_id->valuestring : NULL);
        if (!session->messages[i].tool_call_id && tool_call_id && tool_call_id->valuestring) goto partial_fail_msg;
        session->messages[i].tool_name = str_dup(tool_name && tool_name->valuestring ? tool_name->valuestring : NULL);
        if (!session->messages[i].tool_name && tool_name && tool_name->valuestring) goto partial_fail_msg;
        session->messages[i].error_category = str_dup(error_cat && error_cat->valuestring ? error_cat->valuestring : NULL);
        if (!session->messages[i].error_category && error_cat && error_cat->valuestring) goto partial_fail_msg;
        session->messages[i].thinking = str_dup(thinking && thinking->valuestring ? thinking->valuestring : NULL);
        if (!session->messages[i].thinking && thinking && thinking->valuestring) goto partial_fail_msg;
        session->messages[i].phase = str_dup(
            phase && phase->valuestring ? phase->valuestring : NULL);
        if (!session->messages[i].phase && phase && phase->valuestring)
            goto partial_fail_msg;
        session->messages[i].provider_state = str_dup(
            provider_state && provider_state->valuestring ?
                provider_state->valuestring : NULL);
        if (!session->messages[i].provider_state && provider_state &&
            provider_state->valuestring)
            goto partial_fail_msg;
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
                    if (!session->messages[i].tool_calls[j].id && tc_id && tc_id->valuestring) goto partial_fail_tc;
                    session->messages[i].tool_calls[j].name = str_dup(tc_name && tc_name->valuestring ? tc_name->valuestring : NULL);
                    if (!session->messages[i].tool_calls[j].name && tc_name && tc_name->valuestring) goto partial_fail_tc;
                    if (tc_args)
                    {
                        char *arg_str = cJSON_PrintUnformatted(tc_args);
                        session->messages[i].tool_calls[j].arguments = arg_str;
                        if (!arg_str && tc_args) goto partial_fail_tc;
                    }
                    session->messages[i].tool_calls[j].result_content = str_dup(tc_rc && tc_rc->valuestring ? tc_rc->valuestring : NULL);
                    if (!session->messages[i].tool_calls[j].result_content && tc_rc && tc_rc->valuestring) goto partial_fail_tc;
                    session->messages[i].tool_calls[j].result_error = str_dup(tc_re && tc_re->valuestring ? tc_re->valuestring : NULL);
                    if (!session->messages[i].tool_calls[j].result_error && tc_re && tc_re->valuestring) goto partial_fail_tc;
                }
            }
        }
    }

    cJSON_Delete(arr);
    return 0;

partial_fail_tc:
    /* message_clear frees all fields of message i including the partially-built
     * tool_calls array; tool_calls_count was set at calloc time so every
     * slot up to tc_count is safe for tool_call_free. Leave tool_calls_count
     * as-is so message_clear can iterate and free all slots. */
    message_clear(&session->messages[i]);

partial_fail_msg:
    /* free every message up to (but not including) current index i.
     * message k < i is fully-built; message_clear frees all owned strings
     * and tool_calls. message i (if reached via partial_fail_tc) was already
     * cleared above. message_clear is NULL-safe and idempotent on a
     * calloc-zero struct (the case when i==0 and partial_fail_msg is reached
     * directly). */
    for (int k = 0; k < i; k++)
        message_clear(&session->messages[k]);
    free(session->messages);
    session->messages = NULL;
    session->messages_count = 0;
    cJSON_Delete(arr);
    return -1;
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

/* Parse `json_str` as a JSON object and replace `session->metadata` with the
 * result. `session->metadata` is overwritten unconditionally (the prior
 * cJSON tree is deleted if non-NULL). On parse failure the metadata is
 * reset to an empty `cJSON_CreateObject()` so the non-NULL invariant is
 * preserved. Returns 0 on success, -1 on parse failure (session->metadata
 * is always non-NULL on return — empty-object placeholder on failure). */
int session_deserialize_metadata(Session *session, const char *json_str)
{
    if (!json_str) return -1;
    if (session->metadata) cJSON_Delete(session->metadata);
    session->metadata = cJSON_Parse(json_str);
    if (!session->metadata) { session->metadata = cJSON_CreateObject(); return -1; }
    return 0;
}

/* F5: serialize `session->events` as a NUL-terminated JSON array string.
 * `session` must be non-NULL. Returns NULL iff `session->events` is NULL
 * (legitimate "no events" — save path's bind-null branch is correct here).
 * For a non-NULL empty array, returns a malloc'd `"[]"` so the wire
 * representation is well-formed. Caller `free`s the returned string. The
 * C13 fix treats a NULL return here as legitimate only when
 * `session->events` is NULL — if `session->events` is non-NULL and this
 * returns NULL, it is a cJSON-print OOM and `save_session_core` refuses
 * the save. */
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

/* Parse `json_str` as a JSON array and replace `session->events` with the
 * result. `session->events` is overwritten unconditionally (prior cJSON
 * tree deleted if non-NULL). On parse failure or non-array input, events is
 * reset to an empty `cJSON_CreateArray()` so the non-NULL invariant is
 * preserved. Returns 0 on success, -1 on parse/type failure (session->events
 * is always non-NULL and an array on return). */
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
