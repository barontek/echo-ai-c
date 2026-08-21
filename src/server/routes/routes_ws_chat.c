/*
 * routes_ws_chat.c - per-connection chat context lifecycle:
 * queueing, done-frame emission, branch/fork runs, and
 * session preloading.
 * Depends on: cJSON, agent, session_manager, session_branch.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <cjson/cJSON.h>

#include "routes_ws_internal.h"
#include "../websocket.h"
#include "../../agent/agent.h"
#include "../../agent/message.h"
#include "../../session/session_manager.h"
#include "../../session/session_branch.h"
#include "../../utils/logging.h"
#include "../../utils/string_utils.h"

#ifdef ROUTES_WS_TEST
/* Fault-injection shims (counters live in routes_ws.c): route
 * this TU's calloc/str_dup through the shared hooks so
 * routes_ws_test_set_alloc_fail() reaches the whole module. */
void *routes_ws_test_calloc(size_t nmemb, size_t size);
char *routes_ws_test_strdup(const char *s);
#define calloc routes_ws_test_calloc
#define str_dup routes_ws_test_strdup
#endif


void ws_chat_ctx_destroy(WSChatCtx *c)
{
    if (!c) return;
    if (c->server_ctx)
    {
        WSChatCtx **link = &c->server_ctx->ws_chat_contexts;
        while (*link && *link != c) link = &(*link)->next;
        if (*link == c) *link = c->next;
    }
    if (c->agent) agent_destroy(c->agent);
    session_manager_free(c->sm);
    free(c->pending_request_id);
    free(c->active_session_id);
    free(c->ask_user_response);
    free(c->effort);
    QueuedMsg *q = c->msg_queue;
    while (q)
    {
        QueuedMsg *next = q->next;
        free(q->data);
        free(q);
        q = next;
    }
    free(c);
}

void ws_send_done_forked(WSClient *ws, const char *session_id,
                                   const char *title, LLMResponse *resp,
                                   const char *fork_message_id,
                                   const char *fork_group_id,
                                   const char *fork_role)
{
    cJSON *done = cJSON_CreateObject();
    cJSON_AddStringToObject(done, "type", "done");
    if (resp && resp->content)
        cJSON_AddStringToObject(done, "content", resp->content);
    if (session_id)
        cJSON_AddStringToObject(done, "session_id", session_id);
    if (title)
        cJSON_AddStringToObject(done, "title", title);
    /* After an edit/regenerate fork the edited message's id changes; the
     * frontend keys the branch pill by message id, so it needs the fresh
     * identity to match the following branch_info frame. fork_role tells
     * the frontend WHICH message carries that identity: the edit fork
     * point is the edited user message, while a regenerate fork point is
     * the regenerated assistant response — the pill must land on that
     * message, not on the last assistant message blindly. */
    if (fork_message_id)
        cJSON_AddStringToObject(done, "fork_message_id", fork_message_id);
    if (fork_group_id)
        cJSON_AddStringToObject(done, "fork_group_id", fork_group_id);
    if (fork_role)
        cJSON_AddStringToObject(done, "fork_role", fork_role);
    if (resp)
    {
        cJSON_AddBoolToObject(done, "has_tools", resp->tool_calls_count > 0);
        if (resp->tool_calls_count > 0)
        {
            cJSON *tc_arr = cJSON_CreateArray();
            for (int i = 0; i < resp->tool_calls_count; i++)
            {
                cJSON *tc = cJSON_CreateObject();
                cJSON_AddStringToObject(tc, "name", resp->tool_calls[i].name ? resp->tool_calls[i].name : "");
                cJSON_AddStringToObject(tc, "arguments", resp->tool_calls[i].arguments ? resp->tool_calls[i].arguments : "{}");
                if (resp->tool_calls[i].result_content && resp->tool_calls[i].result_content[0])
                    cJSON_AddStringToObject(tc, "result_content", resp->tool_calls[i].result_content);
                if (resp->tool_calls[i].result_error && resp->tool_calls[i].result_error[0])
                    cJSON_AddStringToObject(tc, "result_error", resp->tool_calls[i].result_error);
                cJSON_AddItemToArray(tc_arr, tc);
            }
            cJSON_AddItemToObject(done, "tool_calls", tc_arr);
        }
    }
    char *str = cJSON_PrintUnformatted(done);
    if (str) ws_send_json(ws, str);
    free(str);
    cJSON_Delete(done);
}

void ws_send_done(WSClient *ws, const char *session_id, const char *title, LLMResponse *resp)
{
    ws_send_done_forked(ws, session_id, title, resp, NULL, NULL, NULL);
}

void ws_chat_flush_queue(WSChatCtx *c)
{
    if (!c || c->ready) return;
    c->ready = 1;

    QueuedMsg *q = c->msg_queue;
    while (q)
    {
        cJSON *json = cJSON_Parse(q->data);
        if (json)
        {
            cJSON *msg = cJSON_GetObjectItem(json, "content");
            if (!msg || !msg->valuestring)
                msg = cJSON_GetObjectItem(json, "message");
            if (msg && msg->valuestring)
            {
                LLMResponse *resp = agent_run_streaming_new(c->agent, msg->valuestring,
                                                        ws_chat_on_chunk, c);
                if (!c->active_session_id && c->agent && c->agent->session_id)
                {
                    free(c->active_session_id);
                    c->active_session_id = str_dup(c->agent->session_id);
                }
                if (resp)
                {
                    ws_send_done(c->ws, c->active_session_id, NULL, resp);
                    llm_response_free(resp);
                }
                else
                {
                    cJSON *err = cJSON_CreateObject();
                    cJSON_AddStringToObject(err, "type", "error");
                    cJSON_AddStringToObject(err, "content", "no response");
                    char *s = cJSON_PrintUnformatted(err);
                    if (s) ws_send_json(c->ws, s);
                    free(s);
                    cJSON_Delete(err);
                }
            }
            cJSON_Delete(json);
        }
        QueuedMsg *next = q->next;
        free(q->data);
        free(q);
        q = next;
    }
    c->msg_queue = NULL;
    c->msg_queue_tail = NULL;
}

void ws_chat_enqueue(WSChatCtx *c, const char *data)
{
    QueuedMsg *q = calloc(1, sizeof(QueuedMsg));
    if (!q)
    {
        /* C11: a dropped queued message was completely silent. */
        log_error("ws: OOM queueing message", NULL);
        return;
    }
    q->data = str_dup(data);
    if (!q->data)
    {
        log_error("ws: OOM duplicating queued message", NULL);
        free(q);
        return;
    }

    if (c->msg_queue_tail)
        c->msg_queue_tail->next = q;
    else
        c->msg_queue = q;
    c->msg_queue_tail = q;
}

void ws_emit_branch_info(WSChatCtx *c)
{
    if (!c || !c->ws || !c->sm || !c->agent || !c->agent->session_id) return;
    char *branches = session_manager_branch_info_alloc(c->sm, c->agent->session_id);
    if (!branches) return;
    cJSON *arr = cJSON_Parse(branches);
    free(branches);
    if (!arr || !cJSON_IsArray(arr))
    {
        if (arr) cJSON_Delete(arr);
        return;
    }
    cJSON *frame = cJSON_CreateObject();
    if (!frame) {
        cJSON_Delete(arr);
        return;
    }
    cJSON_AddStringToObject(frame, "type", "branch_info");
    cJSON_AddItemToObject(frame, "branches", arr);
    char *str = cJSON_PrintUnformatted(frame);
    if (str) ws_send_json(c->ws, str);
    free(str);
    cJSON_Delete(frame);
}

int ws_swap_agent_messages(WSChatCtx *c, Session *s)
{
    if (!c->agent) return -1;
    if (c->agent->messages)
    {
        message_free_all(c->agent->messages, c->agent->messages_count);
        c->agent->messages = NULL;
        c->agent->messages_count = 0;
    }
    if (s->messages_count <= 0) return 0;
    c->agent->messages = calloc((size_t)s->messages_count, sizeof(Message));
    if (!c->agent->messages) return -1;
    for (int i = 0; i < s->messages_count; i++)
    {
        if (message_copy(&c->agent->messages[i], &s->messages[i]) != 0)
        {
            message_free_all(c->agent->messages, i);
            c->agent->messages = NULL;
            c->agent->messages_count = 0;
            return -1;
        }
    }
    c->agent->messages_count = s->messages_count;
    return 0;
}

void ws_run_fork(WSChatCtx *c, const char *message_id, int idx,
                           const char *content, int regen_mode)
{
    SessionManagerForkResult fork_res;
    if (session_manager_fork_branch(c->sm, c->agent->session_id, message_id,
                                    idx, content, &fork_res) != 0)
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "type", "error");
        cJSON_AddStringToObject(err, "content", "fork failed");
        char *s = cJSON_PrintUnformatted(err);
        if (s) ws_send_json(c->ws, s);
        free(s);
        cJSON_Delete(err);
        return;
    }

    /* J2: `idx` is a DB-side index — the in-memory array usually has a
     * `system` message at index 0, so the agent-side keep is offset.
     * L3 defense in depth: never let the keep point below the start of
     * the array (the edit handler validates, but any future caller must
     * not be able to reach messages[-1]). */
    int system_prefix = 0;
    while (system_prefix < c->agent->messages_count &&
           strcmp(c->agent->messages[system_prefix].role, "system") == 0)
        system_prefix++;
    int keep = idx < 0 ? 0 : idx + system_prefix;
    if (keep > c->agent->messages_count)
        keep = c->agent->messages_count;
    for (int i = keep; i < c->agent->messages_count; i++)
        message_clear(&c->agent->messages[i]);
    c->agent->messages_count = keep;

    int append_ok = 1;
    if (!regen_mode)
    {
        /* Append the minted fork message so the run context ends at the
         * fork point (agent_run_streaming_context_new does NOT append a user
         * turn). In regenerate mode the fork copy must NOT be appended:
         * the run would append the response AFTER it and the wholesale
         * agent_save_session would leave the copy as a ghost chain entry
         * (the response is tagged instead, below). */
        Message *fork_msg = calloc(1, sizeof(Message));
        append_ok = 0;
        if (fork_msg && message_copy(fork_msg, &fork_res.fork_message) == 0)
        {
            Message *new_msgs = realloc(c->agent->messages,
                                        sizeof(Message) * (size_t)(keep + 1));
            if (new_msgs)
            {
                c->agent->messages = new_msgs;
                c->agent->messages[keep] = *fork_msg; // NOLINT(clang-analyzer-security.ArrayBound)
                c->agent->messages_count = keep + 1;
                append_ok = 1;
                free(fork_msg);
            }
        }
        if (!append_ok && fork_msg) message_free(fork_msg);
    }
    if (!append_ok)
    {
        /* Context could not carry the fork message — abort the run; the DB
         * fork is already committed and will render on reload. */
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "type", "error");
        cJSON_AddStringToObject(err, "content", "out of memory");
        char *s = cJSON_PrintUnformatted(err);
        if (s) ws_send_json(c->ws, s);
        free(s);
        cJSON_Delete(err);
        message_clear(&fork_res.fork_message);
        free(fork_res.branch_id);
        free(fork_res.fork_message_id);
        free(fork_res.fork_group_id);
        return;
    }

    c->active_runs++;
    LLMResponse *resp = agent_run_streaming_context_new(c->agent, ws_chat_on_chunk, c);
    c->active_runs--;
    if (c->closing)
    {
        llm_response_free(resp);
        message_clear(&fork_res.fork_message);
        free(fork_res.branch_id);
        free(fork_res.fork_message_id);
        free(fork_res.fork_group_id);
        ws_chat_ctx_destroy(c);
        return;
    }
    if (!c->active_session_id && c->agent && c->agent->session_id)
    {
        free(c->active_session_id);
        c->active_session_id = str_dup(c->agent->session_id);
    }

    /* In regenerate mode the fork copy was dropped from the live chain by
     * the wholesale save; tag the fresh response with the fork group and
     * a minted id so branch_info's message_id points at it. On tag failure
     * fall back to the fork copy's id (degraded: the pill won't attach
     * until reload). */
    char *tagged_id = NULL;
    if (resp && regen_mode && c->active_session_id)
        tagged_id = session_manager_tag_message_new(c->sm, c->active_session_id,
                                                idx, fork_res.fork_group_id);

    if (resp)
    {
        ws_send_done_forked(c->ws, c->active_session_id, NULL, resp,
                            tagged_id ? tagged_id : fork_res.fork_message_id,
                            fork_res.fork_group_id,
                            fork_res.fork_message.role);
        llm_response_free(resp);
    }
    else
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "type", "error");
        cJSON_AddStringToObject(err, "content", "no response");
        char *s = cJSON_PrintUnformatted(err);
        if (s) ws_send_json(c->ws, s);
        free(s);
        cJSON_Delete(err);
    }
    ws_emit_branch_info(c);
    free(tagged_id);
    message_clear(&fork_res.fork_message);
    free(fork_res.branch_id);
    free(fork_res.fork_message_id);
    free(fork_res.fork_group_id);
}

void ws_chat_emit_session_start(WSChatCtx *c)
{
    if (!c->agent || !c->agent->session_id) return;
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", "session_start");
    cJSON_AddStringToObject(ev, "session_id", c->agent->session_id);
    char *str = cJSON_PrintUnformatted(ev);
    if (str) ws_send_json(c->ws, str);
    free(str);
    cJSON_Delete(ev);
    c->session_start_emitted = 1;
}

void ws_apply_query_session(WSChatCtx *c, WSClient *ws, const char *query)
{
    (void)ws;
    if (!query || !query[0] || !c->sm || !c->agent) return;
        const char *sid_start = strstr(query, "session_id=");
        if (sid_start)
        {
            sid_start += 11;
            const char *sid_end = strchr(sid_start, '&');
            size_t sid_len = sid_end ? (size_t)(sid_end - sid_start) : strlen(sid_start);
            if (sid_len > 0 && sid_len < 256)
            {
                char session_id[256];
                memcpy(session_id, sid_start, sid_len);
                session_id[sid_len] = '\0';

                free(c->active_session_id);
                c->active_session_id = str_dup(session_id);

                Session *s = session_manager_load_session_alloc(c->sm, session_id);
                if (s)
                {
                    free(c->agent->session_id);
                    c->agent->session_id = str_dup(session_id);

                    /* J5: previously this only emitted a `history` frame to
                     * the client without populating `agent->messages`, so
                     * the first `agent_run_streaming_new` would
                     * `agent_save_session` against a still-empty in-memory
                     * state — silently wiping the DB row's existing
                     * messages on the first turn after reconnect. Now we
                     * mirror the logic that the session_id_item frame path
                     * uses: free prior
                     * agent->messages and copy the loaded session's
                     * messages in. The history frame to the client is
                     * still emitted so the UI shows the prior conversation.
                     * NOTE this does NOT save back — there is nothing to
                     * add yet; the next `agent_run_streaming_new`
                     * (`routes.c:1413`) does `agent_save_session` after
                     * appending the new user turn, which now correctly
                     * saves the full prior+new history instead of just
                     * the new message. */
                    if (c->agent->messages)
                    {
                        message_free_all(c->agent->messages, c->agent->messages_count);
                        c->agent->messages = NULL;
                        c->agent->messages_count = 0;
                    }
                    if (s->messages_count > 0)
                    {
                        c->agent->messages = calloc((size_t)s->messages_count,
                                                    sizeof(Message));
                        if (!c->agent->messages)
                        {
                            /* C6: never run with silently empty history */
                            log_error("ws_apply_query_session: OOM loading "
                                      "messages", NULL);
                            ws_send_json(ws, "{\"type\":\"error\",\"content\":\"out of memory\"}");
                        }
                        else
                        {
                            int copied = 0;
                            for (int i = 0; i < s->messages_count; i++)
                            {
                                if (message_copy(&c->agent->messages[i],
                                                 &s->messages[i]) != 0)
                                {
                                    message_free_all(c->agent->messages, copied);
                                    c->agent->messages = NULL;
                                    c->agent->messages_count = 0;
                                    log_error("ws_apply_query_session: message "
                                              "copy failed", NULL);
                                    ws_send_json(ws, "{\"type\":\"error\",\"content\":\"out of memory\"}");
                                    break;
                                }
                                copied++;
                            }
                            if (c->agent->messages)
                                c->agent->messages_count = s->messages_count;
                        }
                    }

                    if (s->messages_count > 0)
                    {
                        cJSON *hist = cJSON_CreateObject();
                        cJSON_AddStringToObject(hist, "type", "history");
                        cJSON *arr = cJSON_CreateArray();
                        for (int i = 0; i < s->messages_count; i++)
                        {
                            cJSON *m = cJSON_CreateObject();
                            ws_add_message_to_json(m, &s->messages[i]);
                            cJSON_AddItemToArray(arr, m);
                        }
                        cJSON_AddItemToObject(hist, "messages", arr);
                        char *hist_str = cJSON_PrintUnformatted(hist);
                        if (hist_str) ws_send_json(ws, hist_str);
                        free(hist_str);
                        cJSON_Delete(hist);
                    }

                    ws_emit_branch_info(c);

                    session_free(s);
                }
            }
        }
}

void ws_emit_ready(WSChatCtx *c, WSClient *ws)
{
    cJSON *ready = cJSON_CreateObject();
    cJSON_AddStringToObject(ready, "type", "ready");
    if (c->agent && c->agent->session_id)
        cJSON_AddStringToObject(ready, "session_id", c->agent->session_id);
    char *ready_str = cJSON_PrintUnformatted(ready);
    if (ready_str) ws_send_json(ws, ready_str);
    free(ready_str);
    cJSON_Delete(ready);
}
