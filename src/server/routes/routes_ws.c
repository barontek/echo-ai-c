/*
 * routes_ws.c - per-connection WebSocket chat: entry point, auth
 * invalidation, and the fault-injection test hooks. The context
 * lifecycle lives in routes_ws_chat.c, the frame protocol in
 * routes_ws_handlers.c, and the agent callbacks in
 * routes_ws_callbacks.c; shared state in routes_ws_internal.h.
 * Depends on: cJSON, websocket, agent, session_manager, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "routes.h"
#include "routes_ws.h"
#include "routes_ws_internal.h"
#include "../websocket.h"
#include "../../agent/agent.h"
#include "../../config/config.h"
#include "../../utils/logging.h"
#include "../../utils/string_utils.h"

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

void *routes_ws_test_calloc(size_t nmemb, size_t size)
{
    routes_ws_test_alloc_counter++;
    if (routes_ws_test_alloc_counter == routes_ws_test_alloc_fail_at)
        return NULL;
    return calloc(nmemb, size);
}

char *routes_ws_test_strdup(const char *s)
{
    routes_ws_test_alloc_counter++;
    if (routes_ws_test_alloc_counter == routes_ws_test_alloc_fail_at)
        return NULL;
    return str_dup(s);
}

#define calloc routes_ws_test_calloc
#define str_dup routes_ws_test_strdup
#endif



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
