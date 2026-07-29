#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cjson/cJSON.h>

#include "routes.h"
#include "routes_ws.h"
#include "../websocket.h"
#include "../../agent/agent.h"
#include "../../safety/safety.h"
#include "../../session/session_manager.h"
#include "../../utils/logging.h"
#include "../../utils/string_utils.h"
#include "../../tools/registry.h"

typedef struct QueuedMsg {
    char *data;
    struct QueuedMsg *next;
} QueuedMsg;

typedef struct {
    Agent *agent;
    SessionManager *sm;
    SafetyConfig *safety;
    WSClient *ws;
    uv_loop_t *loop;
    char *pending_request_id;
    int approval_done;
    int approval_result;
    int ready;
    QueuedMsg *msg_queue;
    QueuedMsg *msg_queue_tail;
    char *active_session_id;
    int session_start_emitted;
    int ask_user_done;
    char *ask_user_response;
} WSChatCtx;

static void ws_title_update_cb(const char *session_id, const char *title, void *userdata);

static void ws_chat_on_chunk(const char *chunk, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->ws) return;

    cJSON *frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "type", "content");
    cJSON_AddStringToObject(frame, "content", chunk ? chunk : "");
    if (c->active_session_id)
        cJSON_AddStringToObject(frame, "session_id", c->active_session_id);
    char *str = cJSON_PrintUnformatted(frame);
    if (str) ws_send_json(c->ws, str);
    free(str);
    cJSON_Delete(frame);
}

static void ws_send_done(WSClient *ws, const char *session_id, const char *title, LLMResponse *resp)
{
    cJSON *done = cJSON_CreateObject();
    cJSON_AddStringToObject(done, "type", "done");
    if (resp && resp->content)
        cJSON_AddStringToObject(done, "content", resp->content);
    if (session_id)
        cJSON_AddStringToObject(done, "session_id", session_id);
    if (title)
        cJSON_AddStringToObject(done, "title", title);
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

static void ws_chat_flush_queue(WSChatCtx *c)
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
                LLMResponse *resp = agent_run_streaming(c->agent, msg->valuestring,
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

static void ws_chat_enqueue(WSChatCtx *c, const char *data)
{
    QueuedMsg *q = calloc(1, sizeof(QueuedMsg));
    if (!q) return;
    q->data = str_dup(data);
    if (!q->data) { free(q); return; }

    if (c->msg_queue_tail)
        c->msg_queue_tail->next = q;
    else
        c->msg_queue = q;
    c->msg_queue_tail = q;
}

static void ws_chat_on_message(WSClient *ws, const char *data, size_t len, void *userdata)
{
    (void)len;
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->agent) return;

    cJSON *json = cJSON_Parse(data);
    if (!json)
    {
        ws_send_json(ws, "{\"type\":\"error\",\"message\":\"invalid json\"}");
        return;
    }

    cJSON *session_id_item = cJSON_GetObjectItem(json, "session_id");
    if (session_id_item && session_id_item->valuestring)
    {
        if (c->active_session_id)
        {
            if (strcmp(session_id_item->valuestring, c->active_session_id) != 0)
            {
                ws_send_json(ws, "{\"type\":\"error\",\"message\":\"stale session_id\"}");
                cJSON_Delete(json);
                return;
            }
        }
        else if (c->sm && c->agent)
        {
            Session *s = session_manager_load_session(c->sm, session_id_item->valuestring);
            if (s)
            {
                free(c->active_session_id);
                c->active_session_id = str_dup(session_id_item->valuestring);
                free(c->agent->session_id);
                c->agent->session_id = str_dup(session_id_item->valuestring);

                if (c->agent->messages)
                {
                    message_free_all(c->agent->messages, c->agent->messages_count);
                    c->agent->messages = NULL;
                    c->agent->messages_count = 0;
                }
                if (s->messages_count > 0)
                {
                    c->agent->messages = calloc((size_t)s->messages_count, sizeof(Message));
                    if (c->agent->messages)
                    {
                        for (int i = 0; i < s->messages_count; i++)
                        {
                            if (message_copy(&c->agent->messages[i], &s->messages[i]) != 0)
                            {
                                message_free_all(c->agent->messages, i);
                                c->agent->messages = NULL;
                                c->agent->messages_count = 0;
                                break;
                            }
                        }
                        if (c->agent->messages)
                            c->agent->messages_count = s->messages_count;
                    }

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
                /* J3: was previously `}                session_free(s);`
                 * on one line — semantically OK but the visual elision of
                 * `}` and `session_free` on the same column hid the
                 * control-flow boundary. Split onto its own line for
                 * review readability. */
                session_free(s);
            }
            else
            {
                /* I2: the client explicitly named a session_id that does
                 * not exist in the DB. Reject rather than silently ignore
                 * — otherwise the agent would run without ever setting a
                 * session_id and then agent_save_session would remint a
                 * fresh id (per C7), creating a disconnect between the
                 * client's expected session and reality. */
                cJSON_Delete(json);
                ws_send_json(ws, "{\"type\":\"error\",\"message\":\"session not found\"}");
                return;
            }
        }
    }

    cJSON *type_item = cJSON_GetObjectItem(json, "type");

    if (!type_item)
    {
        cJSON *provider = cJSON_GetObjectItem(json, "provider");
        if (provider)
        {
            cJSON *model = cJSON_GetObjectItem(json, "model");
            if (model && model->valuestring && c->agent)
                agent_set_model(c->agent, model->valuestring);
            cJSON_Delete(json);
            cJSON *ready = cJSON_CreateObject();
            cJSON_AddStringToObject(ready, "type", "ready");
            if (c->active_session_id)
                cJSON_AddStringToObject(ready, "session_id", c->active_session_id);
            char *s = cJSON_PrintUnformatted(ready);
            if (s) ws_send_json(ws, s);
            free(s);
            cJSON_Delete(ready);
            return;
        }
        ws_send_json(ws, "{\"type\":\"error\",\"content\":\"missing type\"}");
        cJSON_Delete(json);
        return;
    }

    if (strcmp(type_item->valuestring, "approval_response") == 0)
        {
            cJSON *rid = cJSON_GetObjectItem(json, "request_id");
            cJSON *ok = cJSON_GetObjectItem(json, "approved");
            if (rid && rid->valuestring && ok && cJSON_IsBool(ok) &&
                c->pending_request_id && strcmp(rid->valuestring, c->pending_request_id) == 0)
            {
                c->approval_result = cJSON_IsTrue(ok) ? 1 : 0;
                c->approval_done = 1;
            }
            cJSON_Delete(json);
            return;
        }
        if (strcmp(type_item->valuestring, "ask_user_response") == 0)
        {
            cJSON *answer = cJSON_GetObjectItem(json, "answer");
            if (answer && answer->valuestring)
            {
                free(c->ask_user_response);
                c->ask_user_response = str_dup(answer->valuestring);
                c->ask_user_done = 1;
            }
            else
            {
                c->ask_user_response = str_dup("");
                c->ask_user_done = 1;
            }
            cJSON_Delete(json);
            return;
        }
        if (strcmp(type_item->valuestring, "edit") == 0)
        {
            cJSON *idx_item = cJSON_GetObjectItem(json, "index");
            cJSON *content_item = cJSON_GetObjectItem(json, "content");
            if (idx_item && cJSON_IsNumber(idx_item) && content_item && content_item->valuestring
                && c->sm && c->agent)
            {
                int idx = (int)idx_item->valuedouble;
                int trunc_rc = session_manager_truncate_history(c->sm, c->agent->session_id, idx);
                if (trunc_rc == 0 && c->agent->messages_count > 0)
                {
                    /* J2: `idx` is a DB-side index — i.e. "keep the first
                     * `idx` non-system messages". The in-memory
                     * `agent->messages` array usually has a `system` message
                     * at index 0 (injected by inject_system_with_summary
                     * during agent_run_streaming), so an agent-side keep of
                     * `idx` would drop one message too many — agent keeps
                     * [system, m1..m(idx-2)] while DB keeps [m1..m(idx-1)].
                     * Count the leading system message(s) and offset keep
                     * so both sides end up with the same non-system
                     * messages. */
                    int system_prefix = 0;
                    while (system_prefix < c->agent->messages_count &&
                           strcmp(c->agent->messages[system_prefix].role, "system") == 0)
                        system_prefix++;
                    int keep = idx + system_prefix;
                    if (keep > c->agent->messages_count)
                        keep = c->agent->messages_count;
                    for (int i = keep; i < c->agent->messages_count; i++)
                        message_clear(&c->agent->messages[i]);
                    c->agent->messages_count = keep;
                }

                LLMResponse *resp = agent_run_streaming(c->agent, content_item->valuestring,
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
            return;
        }

        if (strcmp(type_item->valuestring, "stop") == 0)
        {
            agent_cancel(c->agent);
            c->approval_done = 1;
            c->ask_user_done = 1;
            cJSON_Delete(json);
            return;
        }

        if (strcmp(type_item->valuestring, "message") != 0)
        {
            cJSON_Delete(json);
            return;
        }

    cJSON *msg = cJSON_GetObjectItem(json, "content");
    if (!msg || !msg->valuestring)
        msg = cJSON_GetObjectItem(json, "message");
    if (!msg || !msg->valuestring)
    {
        cJSON_Delete(json);
        ws_send_json(ws, "{\"type\":\"error\",\"content\":\"missing message content\"}");
        return;
    }

    if (!c->ready)
    {
        ws_chat_enqueue(c, data);
        cJSON_Delete(json);
        return;
    }

    LLMResponse *resp = agent_run_streaming(c->agent, msg->valuestring,
                                            ws_chat_on_chunk, c);
    cJSON_Delete(json);

    /* agent may have created a session during the run — capture it */
    if (!c->active_session_id && c->agent && c->agent->session_id)
    {
        free(c->active_session_id);
        c->active_session_id = str_dup(c->agent->session_id);
    }

    if (resp)
    {
        ws_send_done(ws, c->active_session_id, NULL, resp);
        llm_response_free(resp);
    }
    else
    {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "type", "error");
        cJSON_AddStringToObject(err, "content", "agent returned no response");
        char *s = cJSON_PrintUnformatted(err);
        if (s) ws_send_json(ws, s);
        free(s);
        cJSON_Delete(err);
    }
}

static void ws_title_update_cb(const char *session_id, const char *title, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->ws) return;

    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", "title_updated");
    cJSON_AddStringToObject(ev, "session_id", session_id ? session_id : "");
    cJSON_AddStringToObject(ev, "title", title ? title : "");
    char *str = cJSON_PrintUnformatted(ev);
    if (str) ws_send_json(c->ws, str);
    free(str);
    cJSON_Delete(ev);
}

static void ws_tool_start_cb(const char *tool_name, const char *arguments, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->ws) return;

    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", "tool_start");
    cJSON_AddStringToObject(ev, "tool_name", tool_name ? tool_name : "");
    cJSON_AddStringToObject(ev, "arguments", arguments ? arguments : "{}");
    char *str = cJSON_PrintUnformatted(ev);
    if (str) ws_send_json(c->ws, str);
    free(str);
    cJSON_Delete(ev);
}

static void ws_tool_end_cb(const char *tool_name, const char *tool_call_id,
                            const char *result_content, const char *result_error,
                            void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->ws) return;

    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", "tool_end");
    cJSON_AddStringToObject(ev, "tool_name", tool_name ? tool_name : "");
    cJSON_AddStringToObject(ev, "tool_call_id", tool_call_id ? tool_call_id : "");
    cJSON_AddStringToObject(ev, "result_content", result_content ? result_content : "");
    cJSON_AddStringToObject(ev, "result_error", result_error ? result_error : "");
    char *str = cJSON_PrintUnformatted(ev);
    if (str) ws_send_json(c->ws, str);
    free(str);
    cJSON_Delete(ev);
}

static void ws_chat_on_close(WSClient *ws, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (c)
    {
        ws->on_close = NULL;
        ws->userdata = NULL;
        c->approval_done = 1;
        c->approval_result = 0;
        /* D2: per-connection Agent — cancel any in-flight streaming, then
         * tear down the owned Agent (and its LLMProvider). This replaces
         * the old code that cancelled the shared ctx->agent (a cross-client
         * bug). */
        if (c->agent)
        {
            agent_cancel(c->agent);
            agent_destroy(c->agent);
            c->agent = NULL;
        }
        free(c->pending_request_id);
        free(c->active_session_id);
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
}

static int ws_approval_cb(const char *tool_name, const char *arguments, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->ws) return 0;

    char *req_id = NULL;
    static unsigned long approval_counter = 0;
    approval_counter++;
    if (asprintf(&req_id, "apr_%lu", approval_counter) < 0) return 0;

    free(c->pending_request_id);
    c->pending_request_id = str_dup(req_id);
    c->approval_done = 0;
    c->approval_result = 0;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "approval_request");
    cJSON_AddStringToObject(req, "request_id", req_id);
    cJSON_AddStringToObject(req, "tool_name", tool_name);
    cJSON_AddStringToObject(req, "arguments", arguments);
    char *req_str = cJSON_PrintUnformatted(req);
    if (req_str) ws_send_json(c->ws, req_str);
    free(req_str);
    cJSON_Delete(req);

    while (!c->approval_done && c->ws && c->loop)
        uv_run(c->loop, UV_RUN_NOWAIT);

    int approved = c->approval_result;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "approval_response");
    cJSON_AddStringToObject(resp, "request_id", req_id);
    cJSON_AddBoolToObject(resp, "approved", approved);
    char *resp_str = cJSON_PrintUnformatted(resp);
    if (resp_str) ws_send_json(c->ws, resp_str);
    free(resp_str);
    cJSON_Delete(resp);
    free(req_id);

    return approved;
}

static char *ws_ask_user_cb(const char *question, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->ws) return NULL;

    c->ask_user_done = 0;
    free(c->ask_user_response);
    c->ask_user_response = NULL;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "ask_user");
    cJSON_AddStringToObject(req, "question", question ? question : "");
    char *req_str = cJSON_PrintUnformatted(req);
    if (req_str) ws_send_json(c->ws, req_str);
    free(req_str);
    cJSON_Delete(req);

    while (!c->ask_user_done && c->ws && c->loop)
        uv_run(c->loop, UV_RUN_NOWAIT);

    char *answer = c->ask_user_response ? str_dup(c->ask_user_response) : NULL;
    return answer;
}

static void ws_chat_emit_session_start(WSChatCtx *c)
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

void routes_ws_chat_init(WSClient *ws, ServerContext *ctx, const char *query)
{
    WSChatCtx *c = calloc(1, sizeof(WSChatCtx));
    if (!c) return;

    /* D2: mint per-connection Agent instead of sharing ctx->agent.
     * Each WS client owns its own messages, session_id, and callbacks. */
    c->agent = agent_create(&ctx->agent_cfg);
    if (!c->agent)
    {
        log_error("ws_chat_init: agent_create failed", NULL);
        free(c);
        return;
    }
    c->sm = ctx->sm;
    c->safety = ctx->safety;
    c->ws = ws;
    c->loop = ctx->loop;

    ws->on_message = ws_chat_on_message;
    ws->on_close = ws_chat_on_close;
    ws->userdata = c;

    /* Wire shared read-mostly resources onto the per-connection Agent.
     * These mirror what main.c sets on the REST-path shared agent. */
    if (c->sm)
        agent_set_session_manager(c->agent, c->sm);
    if (ctx->metrics)
        agent_set_metrics(c->agent, ctx->metrics);

    /* D2: callbacks now act on the per-connection Agent, not the shared one */
    agent_set_approval_callback(c->agent, ws_approval_cb, c);
    agent_set_title_callback(c->agent, ws_title_update_cb, c);
    agent_set_tool_start_callback(c->agent, ws_tool_start_cb, c);
    agent_set_tool_end_callback(c->agent, ws_tool_end_cb, c);
    agent_set_safety(c->agent, c->safety);

    registry_set_ask_user_callback(ws_ask_user_cb, c);

    if (query && query[0] && c->sm && c->agent)
    {
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

                Session *s = session_manager_load_session(c->sm, session_id);
                if (s)
                {
                    free(c->agent->session_id);
                    c->agent->session_id = str_dup(session_id);

                    /* J5: previously this only emitted a `history` frame to
                     * the client without populating `agent->messages`, so
                     * the first `agent_run_streaming` would
                     * `agent_save_session` against a still-empty in-memory
                     * state — silently wiping the DB row's existing
                     * messages on the first turn after reconnect. Now we
                     * mirror the logic that the in-line session_id_item
                     * path below (around line 1249) uses: free prior
                     * agent->messages and copy the loaded session's
                     * messages in. The history frame to the client is
                     * still emitted so the UI shows the prior conversation.
                     * NOTE this does NOT save back — there is nothing to
                     * add yet; the next `agent_run_streaming`
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
                        if (c->agent->messages)
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

                    session_free(s);
                }
            }
        }
    }

    if (!c->active_session_id && c->agent && c->agent->session_id)
        c->active_session_id = str_dup(c->agent->session_id);

    ws_chat_emit_session_start(c);

    cJSON *ready = cJSON_CreateObject();
    cJSON_AddStringToObject(ready, "type", "ready");
    if (c->agent && c->agent->session_id)
        cJSON_AddStringToObject(ready, "session_id", c->agent->session_id);
    char *ready_str = cJSON_PrintUnformatted(ready);
    if (ready_str) ws_send_json(ws, ready_str);
    free(ready_str);
    cJSON_Delete(ready);

    ws_chat_flush_queue(c);
}
