/*
 * routes_ws_handlers.c - the WebSocket message protocol: frame
 * dispatch plus the session_id, provider-switch, regenerate,
 * branch-switch, and message handlers.
 * Depends on: cJSON, agent, provider, safety, session_manager.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <cjson/cJSON.h>

#include "routes_ws_internal.h"
#include "routes.h"
#include "../websocket.h"
#include "../../agent/agent.h"
#include "../../agent/message.h"
#include "../../llm/provider.h"
#include "../../config/config.h"
#include "../../session/session_manager.h"
#include "../../utils/logging.h"
#include "../../utils/string_utils.h"

#ifdef ROUTES_WS_TEST
/* Fault-injection shims (counters live in routes_ws.c). */
void *routes_ws_test_calloc(size_t nmemb, size_t size);
char *routes_ws_test_strdup(const char *s);
#define calloc routes_ws_test_calloc
#define str_dup routes_ws_test_strdup
#endif


int ws_handle_session_id(WSChatCtx *c, WSClient *ws, cJSON *json)
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
                    if (!c->agent->messages)
                    {
                        /* C6: an empty context would make the agent run
                         * with zero history, silently. Fail loudly and
                         * refuse the session instead. */
                        log_error("ws: OOM loading session messages",
                                  "session_id", session_id_item->valuestring, NULL);
                        ws_send_json(ws, "{\"type\":\"error\",\"content\":\"out of memory\"}");
                    }
                    else
                    {
                        for (int i = 0; i < s->messages_count; i++)
                        {
                            if (message_copy(&c->agent->messages[i], &s->messages[i]) != 0)
                            {
                                message_free_all(c->agent->messages, i);
                                c->agent->messages = NULL;
                                c->agent->messages_count = 0;
                                log_error("ws: message copy failed",
                                          "session_id", session_id_item->valuestring, NULL);
                                ws_send_json(ws, "{\"type\":\"error\",\"content\":\"out of memory\"}");
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

void ws_handle_provider_frame(WSChatCtx *c, WSClient *ws, cJSON *json)
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
            {
                if (agent_set_model(c->agent, model->valuestring) != 0)
                {
                    cJSON *err = cJSON_CreateObject();
                    cJSON_AddStringToObject(err, "type", "error");
                    cJSON_AddStringToObject(err, "content",
                                            "model switch failed");
                    char *es = cJSON_PrintUnformatted(err);
                    if (es) ws_send_json(ws, es);
                    free(es);
                    cJSON_Delete(err);
                    cJSON_Delete(json);
                    return;
                }
            }
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

void ws_handle_regenerate(WSChatCtx *c, WSClient *ws, cJSON *json)
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
                         {
                            fi = i - system_prefix;
                            break;
                        }
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

void ws_handle_branch_switch(WSChatCtx *c, WSClient *ws, cJSON *json)
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
                        if (swap_rc != 0)
                        {
                            /* C6: a failed swap must not leave the agent
                             * running with zero history after a
                             * "successful" branch switch. */
                            log_error("ws: branch switch message swap failed",
                                      "session_id", c->active_session_id, NULL);
                            cJSON *err = cJSON_CreateObject();
                            cJSON_AddStringToObject(err, "type", "error");
                            cJSON_AddStringToObject(err, "content",
                                                    "out of memory");
                            char *es = cJSON_PrintUnformatted(err);
                            if (es) ws_send_json(c->ws, es);
                            free(es);
                            cJSON_Delete(err);
                        }
                        else if (s->messages_count > 0)
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

void ws_handle_message_frame(WSChatCtx *c, WSClient *ws, cJSON *json,
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

void ws_chat_on_message(WSClient *ws, const char *data, size_t len, void *userdata)
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
                /* L3: the index is attacker-supplied. A negative value or a
                 * huge/high-precision double would corrupt the context
                 * truncation below (messages[-1] underflow), so it must be
                 * an exact non-negative integer before it reaches the fork
                 * path. */
                double idx_dv = idx_item->valuedouble;
                if (idx_dv < 0 || idx_dv > INT_MAX || idx_dv != (double)(int)idx_dv)
                {
                    cJSON *err = cJSON_CreateObject();
                    cJSON_AddStringToObject(err, "type", "error");
                    cJSON_AddStringToObject(err, "content", "invalid index");
                    char *s = cJSON_PrintUnformatted(err);
                    if (s) ws_send_json(c->ws, s);
                    free(s);
                    cJSON_Delete(err);
                    cJSON_Delete(json);
                    return;
                }
                int idx = (int)idx_dv;
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
