/*
 * callbacks.c - synchronous dispatch registry for run/LLM/tool lifecycle
 * hooks (start/end/error). Depends on: callbacks.h, stdlib.
 */

#define _GNU_SOURCE
#include <stdlib.h>

#include "callbacks.h"

CallbackManager *cb_manager_create(void)
{
    CallbackManager *mgr = calloc(1, sizeof(CallbackManager));
    return mgr;
}

void cb_manager_destroy(CallbackManager *mgr)
{
    free(mgr);
}

int cb_manager_register(CallbackManager *mgr, CallbackHooks hooks)
{
    if (!mgr || mgr->count >= MAX_CALLBACKS) return -1;
    mgr->hooks[mgr->count++] = hooks;
    return 0;
}

void cb_manager_run_start(CallbackManager *mgr, const char *run_id, const char *prompt)
{
    if (!mgr) return;
    for (int i = 0; i < mgr->count; i++)
    {
        if (mgr->hooks[i].on_run_start)
            mgr->hooks[i].on_run_start(run_id, prompt, mgr->hooks[i].userdata);
    }
}

void cb_manager_run_end(CallbackManager *mgr, const char *run_id, const char *response)
{
    if (!mgr) return;
    for (int i = 0; i < mgr->count; i++)
    {
        if (mgr->hooks[i].on_run_end)
            mgr->hooks[i].on_run_end(run_id, response, mgr->hooks[i].userdata);
    }
}

void cb_manager_run_error(CallbackManager *mgr, const char *run_id, const char *error)
{
    if (!mgr) return;
    for (int i = 0; i < mgr->count; i++)
    {
        if (mgr->hooks[i].on_run_error)
            mgr->hooks[i].on_run_error(run_id, error, mgr->hooks[i].userdata);
    }
}

void cb_manager_llm_start(CallbackManager *mgr, const char *run_id, int message_count)
{
    if (!mgr) return;
    for (int i = 0; i < mgr->count; i++)
    {
        if (mgr->hooks[i].on_llm_start)
            mgr->hooks[i].on_llm_start(run_id, message_count, mgr->hooks[i].userdata);
    }
}

void cb_manager_llm_end(CallbackManager *mgr, const char *run_id, const char *response)
{
    if (!mgr) return;
    for (int i = 0; i < mgr->count; i++)
    {
        if (mgr->hooks[i].on_llm_end)
            mgr->hooks[i].on_llm_end(run_id, response, mgr->hooks[i].userdata);
    }
}

void cb_manager_tool_start(CallbackManager *mgr, const char *run_id, const char *tool_name, const char *args)
{
    if (!mgr) return;
    for (int i = 0; i < mgr->count; i++)
    {
        if (mgr->hooks[i].on_tool_start)
            mgr->hooks[i].on_tool_start(run_id, tool_name, args, mgr->hooks[i].userdata);
    }
}

void cb_manager_tool_end(CallbackManager *mgr, const char *run_id, const char *tool_name, const char *result)
{
    if (!mgr) return;
    for (int i = 0; i < mgr->count; i++)
    {
        if (mgr->hooks[i].on_tool_end)
            mgr->hooks[i].on_tool_end(run_id, tool_name, result, mgr->hooks[i].userdata);
    }
}

void cb_manager_tool_error(CallbackManager *mgr, const char *run_id, const char *tool_name, const char *error)
{
    if (!mgr) return;
    for (int i = 0; i < mgr->count; i++)
    {
        if (mgr->hooks[i].on_tool_error)
            mgr->hooks[i].on_tool_error(run_id, tool_name, error, mgr->hooks[i].userdata);
    }
}
