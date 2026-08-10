/*
 * routes_ws.c - per-connection WebSocket chat: the WSChatCtx lifecycle,
 * the message protocol (chat, provider switch, approval, ask_user, edit,
 * regenerate, branch_switch, stop), and JSON frame emission.
 * Depends on: cJSON, websocket, agent, provider registry, safety,
 * session_manager, logging, string_utils, tools/registry.
 */

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
#include "../../llm/provider.h"
#include "../../safety/safety.h"
#include "../../session/session_manager.h"
#include "../../session/session_branch.h"
#include "../../utils/logging.h"
#include "../../utils/string_utils.h"
#include "../../tools/registry.h"

#ifdef ROUTES_WS_TEST
#define WS_STATIC
#else
#define WS_STATIC static
#endif

#ifdef ROUTES_WS_TEST
/* Test-only allocator fault injection: shared counter across calloc and
 * str_dup so tests can fail the Nth allocation inside ws_chat_enqueue
 * (calloc of the node, then the data copy). Only the test target defines
 * ROUTES_WS_TEST. */
static int routes_ws_test_alloc_counter = 0;
static int routes_ws_test_alloc_fail_at = -1;

void routes_ws_test_set_alloc_fail(int nth_allocation)
{
    routes_ws_test_alloc_counter = 0;
    routes_ws_test_alloc_fail_at = nth_allocation;
}

static void *routes_ws_test_calloc(size_t nmemb, size_t size)
{
    routes_ws_test_alloc_counter++;
    if (routes_ws_test_alloc_counter == routes_ws_test_alloc_fail_at)
        return NULL;
    return calloc(nmemb, size);
}

static char *routes_ws_test_strdup(const char *s)
{
    routes_ws_test_alloc_counter++;
    if (routes_ws_test_alloc_counter == routes_ws_test_alloc_fail_at)
        return NULL;
    return str_dup(s);
}

#define calloc routes_ws_test_calloc
#define str_dup routes_ws_test_strdup
#endif

typedef struct QueuedMsg {
    char *data;
    struct QueuedMsg *next;
} QueuedMsg;

typedef struct WSChatCtx {
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
    int ask_user_timeout;
    /* Provider-switch params copied from ServerContext; base_url aliases
     * ctx->agent_cfg (ServerContext outlives every connection, so there is
     * no ownership transfer). api_token aliases ctx->conf. effort is the
     * exception: it is owned because a config-message effort override may
     * replace it mid-connection. */
    const char *base_url;
    const char *api_token;
    int num_ctx;
    int keep_alive_secs;
    char *effort;
    int active_runs;
    int closing;
    ServerContext *server_ctx;
    unsigned long auth_generation;
    struct WSChatCtx *next;
} WSChatCtx;

WS_STATIC void ws_title_update_cb(const char *session_id, const char *title, void *userdata);

static void ws_chat_ctx_destroy(WSChatCtx *c)
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

void routes_ws_invalidate_auth(ServerContext *ctx)
{
    if (!ctx) return;
    for (WSChatCtx *c = ctx->ws_chat_contexts; c; c = c->next)
    {
        if (c->agent)
        {
            agent_cancel(c->agent);
            agent_set_session_manager(c->agent, NULL);
        }
        session_manager_free(c->sm);
        c->sm = NULL;
        c->approval_done = 1;
        c->approval_result = 0;
        c->ask_user_done = 1;
    }
}

WS_STATIC void ws_chat_on_chunk(const char *chunk, void *userdata)
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

WS_STATIC void ws_send_done_forked(WSClient *ws, const char *session_id,
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

WS_STATIC void ws_send_done(WSClient *ws, const char *session_id, const char *title, LLMResponse *resp)
{
    ws_send_done_forked(ws, session_id, title, resp, NULL, NULL, NULL);
}

WS_STATIC void ws_chat_flush_queue(WSChatCtx *c)
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

WS_STATIC void ws_chat_enqueue(WSChatCtx *c, const char *data)
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

/* Emits {"type":"branch_info","branches":[{message_id,count,active},...]}
 * for the active session's live chain. Silent on OOM/absence (the pill just
 * doesn't render until the next frame). */
WS_STATIC void ws_emit_branch_info(WSChatCtx *c)
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
    if (!frame) { cJSON_Delete(arr); return; }
    cJSON_AddStringToObject(frame, "type", "branch_info");
    cJSON_AddItemToObject(frame, "branches", arr);
    char *str = cJSON_PrintUnformatted(frame);
    if (str) ws_send_json(c->ws, str);
    free(str);
    cJSON_Delete(frame);
}

/* Frees agent->messages and deep-copies s->messages in (mirrors the
 * selectSession block). Returns 0 on success, -1 on OOM (agent context is
 * empty afterwards). */
WS_STATIC int ws_swap_agent_messages(WSChatCtx *c, Session *s)
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

/* Shared body of the edit and regenerate handlers: forks the DB chain at
 * `idx` (DB-side index), truncates the agent's in-memory context to the
 * fork point (J2 system-prefix offset), and runs the streaming loop
 * against that context. content == NULL means regenerate. regen_mode
 * decides where the fork point is and what the pill attaches to:
 *  - edit (regen_mode == 0): the fork point is the edited message itself
 *    (a user message). The minted fork copy is appended as the last
 *    context entry and the run appends the fresh assistant response after
 *    it — the copy persists in the DB live chain, so its minted identity
 *    is what branch_info reports.
 *  - regenerate (regen_mode == 1): the fork point is the regenerated
 *    assistant response. agent_run_loop only ever APPENDS, so appending
 *    the fork copy would leave it as a ghost in the DB after the wholesale
 *    agent_save_session. Instead the context is truncated at the fork
 *    point WITHOUT the copy; the run appends the new response, and
 *    session_manager_tag_message_new re-applies the fork group plus a minted
 *    id to it afterwards. Emits done with the fresh fork identity and the
 *    fork point's role (fork_role) so the frontend places the pill on the
 *    right message. */
WS_STATIC void ws_run_fork(WSChatCtx *c, const char *message_id, int idx,
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
     * `system` message at index 0, so the agent-side keep is offset. */
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
                c->agent->messages[keep] = *fork_msg;
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


/* Handles an explicit session_id frame: rejects a stale id with an
 * error frame, or (when no session is active) loads the given session as
 * the new active one. Returns 1 when an error frame was sent and json
 * was consumed — the caller must return — and 0 to continue with the
 * frame body. */
static int ws_handle_session_id(WSChatCtx *c, WSClient *ws, cJSON *json)
{
    cJSON *session_id_item = cJSON_GetObjectItem(json, "session_id");
    if (session_id_item && session_id_item->valuestring)
    {
        if (c->active_session_id)
        {
            if (strcmp(session_id_item->valuestring, c->active_session_id) != 0)
            {
                ws_send_json(ws, "{\"type\":\"error\",\"content\":\"stale session_id\"}");
                cJSON_Delete(json);
                return 1;
            }
        }
        else if (c->sm && c->agent)
        {
            Session *s = session_manager_load_session_alloc(c->sm, session_id_item->valuestring);
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

                    /* D5: do NOT send a `history` event here. The frontend
                     * already loaded this session's messages via REST
                     * (selectSession → api.loadSession) before sending the
                     * first message. Sending history now would overwrite the
                     * user's just-typed message that was added to the UI in
                     * sendMessage(). The init path at line ~711 still sends
                     * history when the WebSocket connects with a
                     * ?session_id= query param. */
                }
                /* J3: was previously `}                session_free(s);`
                 * on one line — semantically OK but the visual elision of
                 * `}` and `session_free` on the same column hid the
                 * control-flow boundary. Split onto its own line for
                 * review readability. */
                ws_emit_branch_info(c);
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
                ws_send_json(ws, "{\"type\":\"error\",\"content\":\"session not found\"}");
                return 1;
            }
        }
    }
    return 0;
}

/* Handles a frame with no "type": a provider/model/effort switch. The
 * frame's json is always consumed (deleted) here. */
static void ws_handle_provider_frame(WSChatCtx *c, WSClient *ws, cJSON *json)
{
    (void)ws;
        cJSON *provider = cJSON_GetObjectItem(json, "provider");
        if (provider)
        {
            if (provider->valuestring && c->agent)
            {
                /* The sidebar sends lm_studio; route it through the local
                 * OpenAI-compatible client rather than OAuth Codex. */
                const char *pname = provider->valuestring;
                if (strcmp(pname, "lm_studio") == 0)
                    pname = "openai_compatible";

                /* Optional reasoning-effort override. Empty string clears
                 * the configured value back to the API default. The new
                 * value is applied below via agent_set_provider, which
                 * rebuilds the provider when it differs from the current
                 * one. Invalid values are rejected loudly instead of being
                 * forwarded to the provider. */
                cJSON *effort_item = cJSON_GetObjectItem(json, "effort");
                if (effort_item)
                {
                    if (cJSON_IsString(effort_item) &&
                        provider_effort_valid(pname,
                                              effort_item->valuestring))
                    {
                        if (effort_item->valuestring[0] == '\0')
                        {
                            /* Empty string clears back to the API default. */
                            free(c->effort);
                            c->effort = NULL;
                        }
                        else
                        {
                            char *copy = str_dup(effort_item->valuestring);
                            if (!copy)
                            {
                                ws_send_json(ws, "{\"type\":\"error\",\"content\":\"out of memory applying effort\"}");
                            }
                            else
                            {
                                free(c->effort);
                                c->effort = copy;
                            }
                        }
                    }
                    else
                    {
                        ws_send_json(ws, "{\"type\":\"error\",\"content\":\"invalid effort value\"}");
                    }
                }

                /* Resolve the per-provider token from [providers]; a
                 * provider switched mid-session may have its own key.
                 * opencode_zen reads the shared "opencode" key. */
                const char *api_token = c->api_token;
                if (c->server_ctx && c->server_ctx->conf)
                {
                    const char *t = conf_provider_token(c->server_ctx->conf, pname);
                    if (t) api_token = t;
                }

                /* Resolve the per-provider base URL the same way startup
                 * does (main.c): [<provider>.base_url] override, else the
                 * provider's canonical default. Never reuse the startup
                 * provider's URL — switching from ollama to opencode_zen
                 * used to keep pointing at localhost:11434, so Zen chats
                 * silently hit the wrong server and returned empty. */
                /* Resolve the per-provider base URL the same way startup
                 * does (main.c): [<provider>.base_url] override, else the
                 * provider's canonical default. Never reuse the startup
                 * provider's URL — switching from ollama to opencode_zen
                 * used to keep pointing at localhost:11434, so Zen chats
                 * silently hit the wrong server and returned empty. */
                const char *base_url = NULL;
                if (c->server_ctx && c->server_ctx->conf)
                {
                    char provider_key[64];
                    snprintf(provider_key, sizeof(provider_key), "%s.base_url",
                             pname);
                    const char *v = conf_get(c->server_ctx->conf, provider_key);
                    if (v) base_url = v;
                }
                if (!base_url) base_url = provider_default_base_url(pname);

                if (agent_set_provider(c->agent, pname, base_url,
                                       api_token,
                                       c->num_ctx, c->keep_alive_secs,
                                       c->effort) != 0)
                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "provider switch failed: %s",
                             provider->valuestring);
                    cJSON *err = cJSON_CreateObject();
                    cJSON_AddStringToObject(err, "type", "error");
                    cJSON_AddStringToObject(err, "content", msg);
                    char *es = cJSON_PrintUnformatted(err);
                    if (es) ws_send_json(ws, es);
                    free(es);
                    cJSON_Delete(err);
                }
            }
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

/* Handles a "regenerate" frame: forks at the given message (by id or
 * index, mapping agent positions past any system prefix) and re-runs the
 * turn. Consumes json. */
static void ws_handle_regenerate(WSChatCtx *c, WSClient *ws, cJSON *json)
{
    (void)ws;
            cJSON *idx_item = cJSON_GetObjectItem(json, "index");
            cJSON *msgid_item = cJSON_GetObjectItem(json, "message_id");
            if (c->sm && c->agent)
            {
                int idx = (idx_item && cJSON_IsNumber(idx_item))
                              ? (int)idx_item->valuedouble : -1;
                /* Resolve the fork index the way fork_branch does: by
                 * message id when provided, else by index. The agent's
                 * array may carry a leading system message, so map agent
                 * positions to DB indices via the system-prefix offset. */
                int system_prefix = 0;
                while (system_prefix < c->agent->messages_count &&
                       strcmp(c->agent->messages[system_prefix].role, "system") == 0)
                    system_prefix++;
                int fi = -1;
                if (msgid_item && msgid_item->valuestring)
                {
                    for (int i = 0; i < c->agent->messages_count; i++)
                    {
                        if (c->agent->messages[i].id &&
                            strcmp(c->agent->messages[i].id,
                                   msgid_item->valuestring) == 0)
                        { fi = i - system_prefix; break; }
                    }
                }
                if (fi < 0) fi = idx;
                /* Regenerating an assistant message forks AT that message:
                 * the fresh response becomes the new chain's fork point
                 * (the pill lands on it via fork_role=assistant). No
                 * step-back — re-running the user turn is what truncating
                 * the context at the fork point already does. */
                if (fi >= 0 && fi + system_prefix < c->agent->messages_count)
                    ws_run_fork(c, NULL, fi, NULL, 1);
                else
                {
                    cJSON *err = cJSON_CreateObject();
                    cJSON_AddStringToObject(err, "type", "error");
                    cJSON_AddStringToObject(err, "content", "invalid index");
                    char *s = cJSON_PrintUnformatted(err);
                    if (s) ws_send_json(c->ws, s);
                    free(s);
                    cJSON_Delete(err);
                }
            }
            cJSON_Delete(json);
            return;
}

/* Handles a "branch_switch" frame: switches the session to the given
 * branch, reloads it, and re-emits history + branch_info. Consumes json. */
static void ws_handle_branch_switch(WSChatCtx *c, WSClient *ws, cJSON *json)
{
    (void)ws;
            cJSON *sid_item = cJSON_GetObjectItem(json, "session_id");
            cJSON *bid_item = cJSON_GetObjectItem(json, "branch_id");
            if (c->sm && c->agent && bid_item && bid_item->valuestring &&
                c->active_session_id &&
                (!sid_item || !sid_item->valuestring ||
                 strcmp(sid_item->valuestring, c->active_session_id) == 0))
            {
                if (session_manager_switch_branch(c->sm, c->active_session_id,
                                                  bid_item->valuestring) == 0)
                {
                    Session *s = session_manager_load_session_alloc(c->sm,
                                                              c->active_session_id);
                    if (s)
                    {
                        int swap_rc = ws_swap_agent_messages(c, s);
                        if (swap_rc == 0 && s->messages_count > 0)
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
                            if (hist_str) ws_send_json(c->ws, hist_str);
                            free(hist_str);
                            cJSON_Delete(hist);
                        }
                        session_free(s);
                        ws_emit_branch_info(c);
                    }
                }
                else
                {
                    cJSON *err = cJSON_CreateObject();
                    cJSON_AddStringToObject(err, "type", "error");
                    cJSON_AddStringToObject(err, "content", "branch not found");
                    char *s = cJSON_PrintUnformatted(err);
                    if (s) ws_send_json(c->ws, s);
                    free(s);
                    cJSON_Delete(err);
                }
            }
            cJSON_Delete(json);
            return;
}

/* Runs the main "message" frame: sends the user text through the agent
 * (streaming) and emits the done/error frame. Consumes json; data is
 * the raw frame text used for queueing when not ready. */
static void ws_handle_message_frame(WSChatCtx *c, WSClient *ws, cJSON *json,
                                    const char *data)
{
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

    c->active_runs++;
    LLMResponse *resp = agent_run_streaming_new(c->agent, msg->valuestring,
                                             ws_chat_on_chunk, c);
    c->active_runs--;
    cJSON_Delete(json);

    if (c->closing)
    {
        llm_response_free(resp);
        ws_chat_ctx_destroy(c);
        return;
    }

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

WS_STATIC void ws_chat_on_message(WSClient *ws, const char *data, size_t len, void *userdata)
{
    (void)len;
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->agent) return;
    if (c->server_ctx &&
        (c->server_ctx->state != STATE_UNLOCKED ||
         c->server_ctx->auth_generation != c->auth_generation))
    {
        ws_send_json(ws, "{\"type\":\"error\",\"content\":\"authentication expired\"}");
        return;
    }

    cJSON *json = cJSON_Parse(data);
    if (!json)
    {
        ws_send_json(ws, "{\"type\":\"error\",\"content\":\"invalid json\"}");
        return;
    }

    if (ws_handle_session_id(c, ws, json) != 0) return;

    cJSON *type_item = cJSON_GetObjectItem(json, "type");

    if (!type_item)
    {
        ws_handle_provider_frame(c, ws, json);
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
            cJSON *msgid_item = cJSON_GetObjectItem(json, "message_id");
            if (idx_item && cJSON_IsNumber(idx_item) && content_item && content_item->valuestring
                && c->sm && c->agent)
            {
                int idx = (int)idx_item->valuedouble;
                ws_run_fork(c, msgid_item && msgid_item->valuestring
                                ? msgid_item->valuestring : NULL,
                            idx, content_item->valuestring, 0);
            }
            cJSON_Delete(json);
            return;
        }

    if (strcmp(type_item->valuestring, "regenerate") == 0)
    {
        ws_handle_regenerate(c, ws, json);
        return;
    }

    if (strcmp(type_item->valuestring, "branch_switch") == 0)
    {
        ws_handle_branch_switch(c, ws, json);
        return;
    }

        if (strcmp(type_item->valuestring, "branch_info") == 0)
        {
            if (c->sm && c->agent)
                ws_emit_branch_info(c);
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

    if (strcmp(type_item->valuestring, "message") != 0)
    {
        cJSON_Delete(json);
        return;
    }

    ws_handle_message_frame(c, ws, json, data);
}

WS_STATIC void ws_title_update_cb(const char *session_id, const char *title, void *userdata)
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

WS_STATIC void ws_tool_start_cb(const char *tool_name, const char *arguments, void *userdata)
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

WS_STATIC void ws_tool_end_cb(const char *tool_name, const char *tool_call_id,
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

WS_STATIC void ws_chat_on_close(WSClient *ws, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (c)
    {
        ws->on_close = NULL;
        ws->userdata = NULL;
        c->closing = 1;
        c->ws = NULL;
        c->approval_done = 1;
        c->approval_result = 0;
        c->ask_user_done = 1;
        if (c->agent) agent_cancel(c->agent);
        if (c->active_runs == 0) ws_chat_ctx_destroy(c);
    }
}

WS_STATIC int ws_approval_cb(const char *tool_name, const char *arguments, void *userdata)
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

WS_STATIC char *ws_ask_user_cb(const char *question, void *userdata)
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

    /* Bound the wait: a client that never answers (or dies mid-block)
     * must not freeze the connection. Configurable via `ask_user_timeout`
     * (seconds, default 60); 0/negative falls back to the default. */
    int timeout_s = c->ask_user_timeout > 0 ? c->ask_user_timeout : 60;
    uint64_t deadline = 0;
    if (c->loop)
        deadline = uv_now(c->loop) + (uint64_t)timeout_s * 1000;

    while (!c->ask_user_done && c->ws && c->loop)
    {
        uv_run(c->loop, UV_RUN_NOWAIT);
        if (uv_now(c->loop) >= deadline) break;
    }

    /* Web mode has no stdin fallback (tool_ask_user reads stdin only when
     * this callback is not registered). Return a placeholder so the tool
     * completes on timeout, connection loss, or `stop` (which sets
     * ask_user_done without a response). */
    if (!c->ask_user_done || !c->ask_user_response)
        return str_dup("(user did not respond)");
    return str_dup(c->ask_user_response);
}

WS_STATIC void ws_chat_emit_session_start(WSChatCtx *c)
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

/* Applies a session_id from the websocket URL query: loads that session
 * into the connection (replacing any in-memory agent messages) and emits
 * its history + branch_info. No-op when query has no session_id or the
 * load fails. */
static void ws_apply_query_session(WSChatCtx *c, WSClient *ws, const char *query)
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

                    ws_emit_branch_info(c);

                    session_free(s);
                }
            }
        }
}

/* Emits the {"type":"ready"} frame once the connection is initialized. */
static void ws_emit_ready(WSChatCtx *c, WSClient *ws)
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
    c->sm = session_manager_retain(ctx->sm);
    c->safety = ctx->safety;
    c->ws = ws;
    c->loop = ctx->loop;
    c->server_ctx = ctx;
    c->auth_generation = ctx->auth_generation;
    c->next = ctx->ws_chat_contexts;
    ctx->ws_chat_contexts = c;

    /* ask_user_timeout (seconds, default 60) bounds how long a blocked
     * ask_user tool call waits for a client reply before giving up. */
    c->ask_user_timeout = ctx->conf
        ? conf_get_int(ctx->conf, "ask_user_timeout", 60) : 60;

    /* Copy the provider-creation params so the config handshake can
     * rebuild the provider when the client switches it mid-session. */
    c->base_url = ctx->agent_cfg.base_url;
    c->api_token = ctx->agent_cfg.api_token;
    c->num_ctx = ctx->agent_cfg.num_ctx;
    c->keep_alive_secs = ctx->agent_cfg.keep_alive_secs;
    c->effort = ctx->agent_cfg.effort ? str_dup(ctx->agent_cfg.effort) : NULL;
    if (ctx->agent_cfg.effort && !c->effort)
        log_error("ws_chat_init: effort copy failed; using API default", NULL);

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

    agent_set_ask_user_callback(c->agent, ws_ask_user_cb, c);

    ws_apply_query_session(c, ws, query);

    if (!c->active_session_id && c->agent && c->agent->session_id)
        c->active_session_id = str_dup(c->agent->session_id);

    ws_chat_emit_session_start(c);

    ws_emit_ready(c, ws);

    ws_chat_flush_queue(c);
}
