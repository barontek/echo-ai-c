/*
 * routes_ws_callbacks.c - agent callbacks wired per connection:
 * chunk streaming, title/tool events, approval, ask_user, and
 * close teardown.
 * Depends on: cJSON, libuv, agent, websocket.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <cjson/cJSON.h>
#include <uv.h>

#include "routes_ws_internal.h"
#include "../websocket.h"
#include "../../agent/agent.h"
#include "../../utils/logging.h"
#include "../../utils/string_utils.h"

#ifdef ROUTES_WS_TEST
/* Fault-injection shims (counters live in routes_ws.c). */
void *routes_ws_test_calloc(size_t nmemb, size_t size);
char *routes_ws_test_strdup(const char *s);
#define calloc routes_ws_test_calloc
#define str_dup routes_ws_test_strdup
#endif


void ws_chat_on_chunk(const char *chunk, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->ws) return;

    cJSON *frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "type", "content");
    cJSON_AddStringToObject(frame, "content", chunk ? chunk : "");
    if (c->active_session_id)
        cJSON_AddStringToObject(frame, "session_id", c->active_session_id);
    char *str = cJSON_PrintUnformatted(frame);
    if (!str)
    {
        /* C11: a dropped stream chunk was completely silent on both
         * transports — the user saw a hole in the answer with no trace. */
        log_error("ws: OOM rendering stream chunk", NULL);
        cJSON_Delete(frame);
        return;
    }
    ws_send_json(c->ws, str);
    free(str);
    cJSON_Delete(frame);
}

void ws_title_update_cb(const char *session_id, const char *title, void *userdata)
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

void ws_tool_start_cb(const char *tool_name, const char *arguments, void *userdata)
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

void ws_tool_end_cb(const char *tool_name, const char *tool_call_id,
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

void ws_chat_on_close(WSClient *ws, void *userdata)
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

int ws_approval_cb(const char *tool_name, const char *arguments, void *userdata)
{
    WSChatCtx *c = (WSChatCtx *)userdata;
    if (!c || !c->ws) return 0;

    char *req_id = NULL;
    /* Loop-thread only (same reasoning as server.c req_counter). */
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

char *ws_ask_user_cb(const char *question, void *userdata)
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
