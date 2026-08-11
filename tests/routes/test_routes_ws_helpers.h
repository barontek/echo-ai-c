/*
 * test_routes_ws_helpers.h - shared stubs and fixtures for the routes_ws
 * test binaries (callbacks/queue/message/provider/fork). The struct
 * mirror and externs that test_routes_ws.c used to carry are replaced by
 * routes_ws_internal.h, which now owns the WSChatCtx layout and the
 * ws_* declarations. Depends on: check, the routes_ws units.
 */

#ifndef ECHO_TEST_ROUTES_WS_HELPERS_H
#define ECHO_TEST_ROUTES_WS_HELPERS_H

#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <cjson/cJSON.h>
#include <uv.h>

#include "../src/server/server.h"
#include "../src/server/websocket.h"
#include "../src/server/routes/routes.h"
#include "../src/server/routes/routes_ws.h"
#include "../src/server/routes/routes_ws_internal.h"
#include "../src/config/config.h"
#include "../src/agent/agent.h"
#include "../src/agent/message.h"
#include "../src/session/session.h"
#include "../src/session/session_manager.h"
#include "../src/session/session_branch.h"
#include "../src/utils/string_utils.h"

extern int captured_ws_send_count;

/* uv_run stub knobs: set before calling a function that blocks on
 * the loop (approval/ask_user); the stub resolves them immediately. */
extern WSChatCtx *g_loop_ctx;
extern int g_want_approval;
extern char *g_want_answer;
extern char captured_ws_json[8192];

/* Monotonic-clock stand-in: first call returns UV_NOW_FIRST (used to
 * compute the deadline), later calls return UV_NOW_LATER — enough to
 * make a 1s ask_user timeout elapse deterministically without real
 * waiting. Declared here so setup() can reset it. */
#define UV_NOW_FIRST 1000
#define UV_NOW_LATER 2001
extern int stub_uv_now_calls;

extern int stub_agent_run_streaming_count;
extern LLMResponse *stub_agent_run_streaming_resp;
extern int stub_streaming_chunk_count;
extern const char *stub_streaming_chunks[4];
extern WSClient *stub_close_ws;
extern WSChatCtx *stub_close_ctx;

extern int stub_agent_create_succeeds;
extern Session *stub_session_load_result;

extern int stub_agent_set_provider_result;
extern int stub_agent_set_provider_calls;
extern const char *stub_agent_set_provider_name;
extern const char *stub_agent_set_provider_base_url;
extern const char *stub_agent_set_provider_token;
extern int stub_agent_set_provider_num_ctx;
extern int stub_agent_set_provider_keep_alive;
extern const char *stub_agent_set_provider_effort;
extern const char *stub_agent_set_model_name;
extern int stub_agent_cancel_calls;
extern int stub_agent_clear_sm_calls;
extern int stub_session_manager_free_calls;

extern LLMResponse fake_resp_basic;
extern LLMResponse fake_resp_with_tools;
extern ToolCall fake_tools[2];
extern Session fake_session;
extern Message fake_session_msgs[1];

extern int stub_fork_rc;
extern const char *stub_fork_branch_id;
extern const char *stub_fork_message_id;
extern const char *stub_fork_group_id;
extern const char *stub_fork_content;
extern int stub_tag_rc;
extern const char *stub_tag_id;
extern int stub_switch_rc;
extern const char *stub_branch_info_json;

void reset_capture(void);
void setup(void);
void teardown(void);

#endif /* ECHO_TEST_ROUTES_WS_HELPERS_H */
