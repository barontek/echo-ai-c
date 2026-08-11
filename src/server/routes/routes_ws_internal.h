/*
 * routes_ws_internal.h - shared state and cross-unit contracts for the
 * WebSocket chat connection, spread across routes_ws (core), chat,
 * handlers, and callbacks units. Documented exception to the
 * one-header-per-module rule: the connection context struct is mutated
 * by all four units and by the test binary, so it lives here instead of
 * being duplicated. Not installed or included outside src/server/routes.
 * Depends on: routes.h, websocket.h, agent.h, provider.h, safety.h,
 * session_manager.h.
 */

#ifndef ECHO_ROUTES_WS_INTERNAL_H
#define ECHO_ROUTES_WS_INTERNAL_H

#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <uv.h>

#include "routes.h"
#include "routes_ws.h"
#include "../websocket.h"
#include "../../agent/agent.h"
#include "../../llm/provider.h"
#include "../../safety/safety.h"
#include "../../session/session_manager.h"
#include "../../session/session_branch.h"
#include "../../utils/string_utils.h"

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

/* routes_ws_chat.c */
void ws_chat_ctx_destroy(WSChatCtx *c);
void ws_send_done_forked(WSClient *ws, const char *session_id,
                         const char *title, LLMResponse *resp,
                         const char *fork_message_id,
                         const char *fork_group_id, const char *fork_role);
void ws_send_done(WSClient *ws, const char *session_id, const char *title,
                  LLMResponse *resp);
void ws_chat_flush_queue(WSChatCtx *c);
void ws_chat_enqueue(WSChatCtx *c, const char *data);
void ws_emit_branch_info(WSChatCtx *c);
int ws_swap_agent_messages(WSChatCtx *c, Session *s);
void ws_run_fork(WSChatCtx *c, const char *message_id, int idx,
                 const char *content, int regen_mode);
void ws_chat_emit_session_start(WSChatCtx *c);
void ws_apply_query_session(WSChatCtx *c, WSClient *ws, const char *query);
void ws_emit_ready(WSChatCtx *c, WSClient *ws);

/* routes_ws_handlers.c */
int ws_handle_session_id(WSChatCtx *c, WSClient *ws, cJSON *json);
void ws_handle_provider_frame(WSChatCtx *c, WSClient *ws, cJSON *json);
void ws_handle_regenerate(WSChatCtx *c, WSClient *ws, cJSON *json);
void ws_handle_branch_switch(WSChatCtx *c, WSClient *ws, cJSON *json);
void ws_handle_message_frame(WSChatCtx *c, WSClient *ws, cJSON *json,
                             const char *data);
void ws_chat_on_message(WSClient *ws, const char *data, size_t len,
                        void *userdata);

/* routes_ws_callbacks.c */
void ws_chat_on_chunk(const char *chunk, void *userdata);
void ws_title_update_cb(const char *session_id, const char *title,
                        void *userdata);
void ws_tool_start_cb(const char *tool_name, const char *arguments,
                      void *userdata);
void ws_tool_end_cb(const char *tool_name, const char *tool_call_id,
                    const char *result_content, const char *result_error,
                    void *userdata);
void ws_chat_on_close(WSClient *ws, void *userdata);
int ws_approval_cb(const char *tool_name, const char *arguments,
                   void *userdata);
char *ws_ask_user_cb(const char *question, void *userdata);

#ifdef ROUTES_WS_TEST
/* Allocation shims defined non-static in routes_ws.c under the same
 * guard; every routes_ws unit compiled with ROUTES_WS_TEST routes its
 * calloc/str_dup through them (via per-TU #defines after includes) so
 * routes_ws_test_set_alloc_fail() reaches the whole module. */
void *routes_ws_test_calloc(size_t nmemb, size_t size);
char *routes_ws_test_strdup(const char *s);
#endif

#endif /* ECHO_ROUTES_WS_INTERNAL_H */
