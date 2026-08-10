/*
 * routes.c - HTTP route table and shared route helpers: message-to-JSON
 * serialization for REST and WS paths, and prefix/exact path matching.
 * Depends on: cJSON, routes_auth/openai_auth/session/chat/general handler
 * modules (omitted under ROUTES_TEST so the test binary links only this).
 */

#define _GNU_SOURCE
#include <string.h>
#include <cjson/cJSON.h>

#include "routes.h"
#ifndef ROUTES_TEST
#include "routes_auth.h"
#include "routes_openai_auth.h"
#include "routes_session.h"
#include "routes_chat.h"
#include "routes_general.h"
#endif

int ws_add_message_to_json(cJSON *m, const Message *msg)
{
    if (!m || !msg) return -1;
    cJSON_AddStringToObject(m, "role", msg->role ? msg->role : "unknown");
    cJSON_AddStringToObject(m, "content", msg->content ? msg->content : "");

    /* Branching identity: emit only when present so legacy chains (no
     * ids minted yet) keep the same wire shape as before. */
    if (msg->id)
        cJSON_AddStringToObject(m, "id", msg->id);

    if (msg->parent_id)
        cJSON_AddStringToObject(m, "parent_id", msg->parent_id);

    if (msg->fork_group_id)
        cJSON_AddStringToObject(m, "fork_group_id", msg->fork_group_id);

    if (msg->thinking)
        cJSON_AddStringToObject(m, "thinking", msg->thinking);

    if (msg->tool_name)
        cJSON_AddStringToObject(m, "tool_name", msg->tool_name);

    if (msg->tool_call_id)
        cJSON_AddStringToObject(m, "tool_call_id", msg->tool_call_id);

    if (msg->error_category)
        cJSON_AddStringToObject(m, "error_category", msg->error_category);

    if (msg->tool_calls && msg->tool_calls_count > 0)
    {
        cJSON *tc_arr = cJSON_CreateArray();
        for (int j = 0; j < msg->tool_calls_count; j++)
        {
            cJSON *tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "name", msg->tool_calls[j].name ? msg->tool_calls[j].name : "");
            cJSON_AddStringToObject(tc, "arguments",
                msg->tool_calls[j].arguments ? msg->tool_calls[j].arguments : "{}");
            if (msg->tool_calls[j].result_content && msg->tool_calls[j].result_content[0])
                cJSON_AddStringToObject(tc, "result_content", msg->tool_calls[j].result_content);
            if (msg->tool_calls[j].result_error && msg->tool_calls[j].result_error[0])
                cJSON_AddStringToObject(tc, "result_error", msg->tool_calls[j].result_error);
            cJSON_AddItemToArray(tc_arr, tc);
        }
        cJSON_AddItemToObject(m, "tool_calls", tc_arr);
        cJSON_AddBoolToObject(m, "has_tools", 1);
    }
    return 0;
}

#ifndef ROUTES_TEST
const Route routes[] = {
    {"GET",  "/api/status",               0, 0, 0, handle_status},
    {"GET",  "/api/health",               0, 0, 0, handle_health},
    {"GET",  "/api/health/detailed",      0, 0, 0, handle_health_detailed},
    {"GET",  "/api/config",               0, 0, 0, handle_config},
    {"POST", "/api/setup",                0, 0, 0, handle_setup},
    {"POST", "/api/unlock",               0, 0, 0, handle_unlock},
    {"POST", "/api/logout",               0, 1, 0, handle_logout},
    {"GET",  "/api/models",               0, 0, 0, handle_models},
    {"GET",  "/api/providers",            0, 0, 0, handle_providers},
    {"GET",  "/api/sessions",             0, 1, 0, handle_sessions},
    {"POST", "/api/sessions",             0, 1, 0, handle_create_session},
    {"POST", "/api/sessions/rename",      0, 1, 0, handle_sessions_rename},
    {"GET",  "/api/sessions/",            1, 1, 0, handle_session_get},
    {"DELETE","/api/sessions/",           1, 1, 0, handle_session_delete},
    {"PUT",  "/api/sessions/",            1, 1, 0, handle_session_update},
    {"POST", "/api/sessions/import",      0, 1, 0, handle_session_import},
    {"POST", "/api/change-password",      0, 1, 0, handle_change_password},
    {"GET",  "/api/auth/openai/status",    0, 1, 0, handle_openai_oauth_status},
    {"POST", "/api/auth/openai/start",     0, 1, 0, handle_openai_oauth_start},
    {"POST", "/api/auth/openai/logout",    0, 1, 0, handle_openai_oauth_logout},
    {"POST", "/api/chat",                 0, 1, 0, handle_chat},
    {"GET",  "/api/stream",               0, 1, 1, handle_sse_stream},
    {"GET",  "/api/metrics",              0, 0, 0, handle_metrics},
    {"POST", "/api/undo",                 0, 1, 0, handle_undo},
    {"POST", "/api/redo",                 0, 1, 0, handle_redo},
};

const int routes_count = sizeof(routes) / sizeof(routes[0]);
#endif

int route_match(const char *method, const char *path, const Route *r)
{
    if (strcmp(method, r->method) != 0) return 0;
    if (r->is_prefix)
        return strncmp(path, r->path, strlen(r->path)) == 0;
    return strcmp(path, r->path) == 0;
}
