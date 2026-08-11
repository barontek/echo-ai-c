/*
 * agent_tools.c - tool-call execution for one agent run:
 * registry dispatch, safety approval, result capping, and
 * tool-result message recording.
 * Depends on: tools/registry, tools/tool, safety, metrics.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "agent_internal.h"
#include "agent_tools.h"
#include "../tools/registry.h"
#include "../tools/tool.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"


int execute_tool_calls(Agent *agent, ToolCall *calls, int count)
{
    double tool_buckets[] = {0.01, 0.05, 0.1, 0.5, 1, 5, 10, 30};

    for (int i = 0; i < count; i++)
    {
        double start = time_sec();
        char *args_str = calls[i].arguments ? calls[i].arguments : "{}";
        const char *tname = calls[i].name ? calls[i].name : "unknown";

        cb_manager_tool_start(agent->cb_mgr, NULL, tname, args_str);

        Tool *tool = registry_get(calls[i].name);
        if (!tool)
        {
            if (agent->metrics)
                metrics_counter_inc(agent->metrics, "echo_tool_errors_total",
                                    "Total tool execution errors by name");
            Message *err_msg = message_create("tool", "tool not found");
            if (!err_msg)
            {
                log_error("execute_tool_calls: OOM building tool-not-found "
                          "message", "tool", tname, NULL);
                cb_manager_tool_error(agent->cb_mgr, NULL, tname,
                                      "tool not found");
                continue;
            }
            err_msg->tool_call_id = str_dup(calls[i].id ? calls[i].id : "");
            err_msg->tool_name = str_dup(tname);
            err_msg->error_category = str_dup("tool_not_found");
            if (agent_append_message(agent, err_msg) == 0)
                free(err_msg); /* struct only: fields moved into the array */
            else
            {
                log_error("execute_tool_calls: OOM appending tool-not-found "
                          "message", "tool", tname, NULL);
                message_free(err_msg);
            }
            cb_manager_tool_error(agent->cb_mgr, NULL, tname, "tool not found");
            continue;
        }

        if (agent->safety && safety_needs_approval(agent->safety, calls[i].name))
        {
            int ok = agent->on_approval
                         ? agent->on_approval(calls[i].name, args_str,
                                              agent->approval_userdata)
                         : 0;
            if (!ok)
            {
                Message *err_msg = message_create("tool", "tool call denied");
                if (!err_msg)
                {
                    log_error("execute_tool_calls: OOM building denial "
                              "message", "tool", tname, NULL);
                    cb_manager_tool_error(agent->cb_mgr, NULL, tname,
                                          "denied");
                    continue;
                }
                err_msg->tool_call_id = str_dup(calls[i].id ? calls[i].id : "");
                err_msg->tool_name = str_dup(tname);
                err_msg->error_category = str_dup("denied");
                if (agent_append_message(agent, err_msg) == 0)
                    free(err_msg); /* struct only: fields moved into array */
                else
                {
                    log_error("execute_tool_calls: OOM appending denial "
                              "message", "tool", tname, NULL);
                    message_free(err_msg);
                }
                cb_manager_tool_error(agent->cb_mgr, NULL, tname, "denied");
                continue;
            }
        }

        if (agent->on_tool_start)
            agent->on_tool_start(tname, args_str, agent->tool_start_userdata);

        registry_set_ask_user_callback(agent->on_ask_user,
                                       agent->ask_user_userdata);
        ToolResult *result = tool->execute(tool, args_str);
        registry_set_ask_user_callback(NULL, NULL);
        if (!result)
            result = tool_result_error("tool returned no result", "execution_error");

        /* Generic safety net: cap oversized tool results so one result
         * cannot blow the whole context window. web_fetch already extracts
         * and truncates; this catches read_file / rest_api / grep etc.
         * On truncation OOM we fall back to the untruncated content
         * (borrowed, not owned) rather than losing the result. */
        const char *raw_content = result->content ? result->content : "";
        const char *display = raw_content;
        char *capped = NULL;
        if (agent->max_tool_result_chars > 0)
        {
            capped = str_truncate_ellipsis_dup(raw_content,
                                           (size_t)agent->max_tool_result_chars);
            if (capped) display = capped;
        }

        free(calls[i].result_content);
        free(calls[i].result_error);
        calls[i].result_content = str_dup(display);
        calls[i].result_error = str_dup(result->error ? result->error : "");

        if (agent->on_tool_end)
            agent->on_tool_end(tname, calls[i].id,
                               calls[i].result_content, calls[i].result_error,
                               agent->tool_end_userdata);

        double elapsed = time_sec() - start;

        if (agent->metrics)
        {
            metrics_histogram_observe(agent->metrics, "echo_tool_duration_seconds",
                                      "Tool execution duration in seconds",
                                      elapsed, tool_buckets, 8);
            if (result->error)
                metrics_counter_inc(agent->metrics, "echo_tool_errors_total",
                                    "Total tool execution errors by name");
        }

        Message *tool_msg = message_create("tool", display);
        if (!tool_msg)
        {
            log_error("execute_tool_calls: OOM building tool message",
                      "tool", tname, NULL);
            free(capped);
            tool_result_free(result);
            continue;
        }
        tool_msg->tool_call_id = str_dup(calls[i].id ? calls[i].id : "");
        tool_msg->tool_name = str_dup(calls[i].name);
        if (result->error)
        {
            tool_msg->error_category = str_dup(result->error_category ? result->error_category : "execution_error");
            char *err_content = NULL;
            if (asprintf(&err_content, "Error: %s", result->error) < 0)
                err_content = str_dup("Error");
            free(tool_msg->content);
            tool_msg->content = err_content;
        }

        if (agent_append_message(agent, tool_msg) == 0)
            free(tool_msg); /* struct only: fields moved into the array */
        else
        {
            log_error("execute_tool_calls: OOM appending tool message",
                      "tool", tname, NULL);
            message_free(tool_msg);
        }
        free(capped);
        if (result->error)
            cb_manager_tool_error(agent->cb_mgr, NULL, tname, result->error);
        else
            cb_manager_tool_end(agent->cb_mgr, NULL, tname, result->content);
        tool_result_free(result);
    }

    return 0;
}

#ifdef AGENT_TEST
int agent_test_execute_tool_calls(Agent *agent, ToolCall *calls, int count)
{
    return execute_tool_calls(agent, calls, count);
}
#endif

