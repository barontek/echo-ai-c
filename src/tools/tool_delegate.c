#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "tool.h"
#include "registry.h"
#include "../utils/string_utils.h"
#include "../agent/message.h"
#include "../llm/provider.h"
#include "../utils/logging.h"

#ifdef TOOL_DELEGATE_TEST
static int td_alloc_counter = 0;
static int td_alloc_fail_at = -1;

void tool_delegate_test_set_alloc_fail(int nth_allocation)
{
    td_alloc_counter = 0;
    td_alloc_fail_at = nth_allocation;
}

static char *td_test_strdup(const char *s)
{
    td_alloc_counter++;
    if (td_alloc_counter == td_alloc_fail_at) return NULL;
    return str_dup(s);
}

#define str_dup td_test_strdup

LLMProvider *td_test_get_provider(const char *name, const char *model,
                                   const char *base_url, int num_ctx, int keep_alive_secs);
#define get_provider td_test_get_provider
#endif

typedef struct {
    SafetyConfig *safety;
} DelegateToolCtx;

static int sub_tool_calls_remaining(ToolCall *calls, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (calls[i].name && calls[i].name[0] != '\0')
            return 1;
    }
    return 0;
}

static ToolResult *delegate_execute(Tool *self, const char *args_json)
{
    (void)self;

    const char *provider_name = NULL;
    const char *base_url = NULL;
    const char *model = NULL;
    int num_ctx = 4096;
    int keep_alive_secs = 120;
    double temperature = 0.7;
    int timeout = 120;
    int max_iterations = 10;

    if (registry_get_delegate_config(&provider_name, &base_url, &model,
                                      &num_ctx, &keep_alive_secs,
                                      &temperature, &timeout, &max_iterations) != 0)
    {
        return tool_result_error("delegate not configured (set agent config first)", "execution_error");
    }

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *task = cJSON_GetObjectItem(args, "task");
    if (!task || !cJSON_IsString(task))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'task' argument", "validation_error");
    }

    int iterations = max_iterations;
    cJSON *iter = cJSON_GetObjectItem(args, "iterations");
    if (iter && cJSON_IsNumber(iter))
        iterations = iter->valuedouble;
    if (iterations < 1) iterations = 1;
    if (iterations > 50) iterations = 50;
    char *task_str = str_dup(cJSON_GetStringValue(task));
    cJSON_Delete(args);
    if (!task_str) return tool_result_error("oom", "execution_error");

    LLMProvider *provider = get_provider(provider_name, model,
                                          base_url, num_ctx, keep_alive_secs);
    if (!provider)
    {
        free(task_str);
        return tool_result_error("failed to create sub-agent provider", "execution_error");
    }

    Message *msgs = NULL;
    int msgs_count = 0;
    int msgs_cap = 0;

    char *sys_role = str_dup("system");
    char *sys_content = str_dup("You are a helpful sub-agent. Complete the task given by the user. "
                                "Use tools when needed. Keep responses concise.");
    char *user_role = str_dup("user");
    if (!sys_role || !sys_content || !user_role)
    {
        free(sys_role); free(sys_content); free(user_role); free(task_str);
        provider->destroy(provider);
        return tool_result_error("oom", "execution_error");
    }
    char *user_content = task_str;
    Message sys_msg = {0};
    sys_msg.role = sys_role;
    sys_msg.content = sys_content;

    Message user_msg = {0};
    user_msg.role = user_role;
    user_msg.content = user_content;

    msgs = malloc(sizeof(Message) * 4);
    if (!msgs) { provider->destroy(provider); return tool_result_error("oom", "execution_error"); }
    msgs[0] = sys_msg;
    msgs[1] = user_msg;
    msgs_count = 2;
    msgs_cap = 4;

    char *final_content = NULL;

    for (int i = 0; i < iterations; i++)
    {
        LLMResponse *resp = provider->chat(provider, msgs, msgs_count,
                                            model, temperature, timeout,
                                            NULL);
        if (!resp)
        {
            log_error("delegate: sub-agent LLM returned no response", NULL);
            break;
        }

        int has_tool_calls = sub_tool_calls_remaining(resp->tool_calls,
                                                       resp->tool_calls_count);

        if (resp->content)
        {
            free(final_content);
            final_content = str_dup(resp->content);
        }

        if (resp->content || has_tool_calls)
        {
            Message *assistant_msg = calloc(1, sizeof(Message));
            if (assistant_msg)
            {
                Message source = {0};
                source.role = "assistant";
                source.content = resp->content ? resp->content : "";
                source.tool_calls = resp->tool_calls;
                source.tool_calls_count = resp->tool_calls_count;
                if (message_copy(assistant_msg, &source) != 0)
                {
                    free(assistant_msg);
                }
                else
                {
                    if (msgs_count >= msgs_cap)
                    {
                        int new_cap = msgs_cap * 2;
                        Message *new_msgs = realloc(msgs, sizeof(Message) * new_cap);
                        if (!new_msgs)
                        {
                            message_clear(assistant_msg);
                            free(assistant_msg);
                            llm_response_free(resp);
                            break;
                        }
                        msgs = new_msgs;
                        msgs_cap = new_cap;
                    }
                    msgs[msgs_count++] = *assistant_msg;
                    free(assistant_msg);
                }
            }
        }

        if (!has_tool_calls)
        {
            llm_response_free(resp);
            break;
        }

        for (int j = 0; j < resp->tool_calls_count; j++)
        {
            ToolCall *tc = &resp->tool_calls[j];
            const char *tname = tc->name ? tc->name : "unknown";
            const char *targs = tc->arguments ? tc->arguments : "{}";

            Tool *tool = registry_get(tname);
            if (!tool)
            {
                Message *tool_msg = calloc(1, sizeof(Message));
                if (tool_msg)
                {
                    char *t_role = str_dup("tool");
                    char *t_content = str_dup("tool not found");
                    char *t_call_id = str_dup(tc->id ? tc->id : "");
                    char *t_name = str_dup(tname);
                    char *t_err_cat = str_dup("tool_not_found");
                    if (!t_role || !t_content || !t_call_id || !t_name || !t_err_cat)
                    {
                        free(t_role); free(t_content); free(t_call_id); free(t_name); free(t_err_cat); free(tool_msg);
                    }
                    else
                    {
                        tool_msg->role = t_role;
                        tool_msg->content = t_content;
                        tool_msg->tool_call_id = t_call_id;
                        tool_msg->tool_name = t_name;
                        tool_msg->error_category = t_err_cat;
                        if (msgs_count >= msgs_cap)
                        {
                            msgs_cap *= 2;
                            Message *new_msgs = realloc(msgs, sizeof(Message) * msgs_cap);
                            if (!new_msgs) { free(t_role); free(t_content); free(t_call_id); free(t_name); free(t_err_cat); free(tool_msg); break; }
                            msgs = new_msgs;
                        }
                        msgs[msgs_count++] = *tool_msg;
                        free(tool_msg);
                    }
                }
                continue;
            }

            ToolResult *result = tool->execute(tool, targs);
            if (!result)
                result = tool_result_error("tool returned no result", "execution_error");

            Message *tool_msg = calloc(1, sizeof(Message));
            if (tool_msg)
            {
                char *t_role = str_dup("tool");
                char *t_content = str_dup(result->content ? result->content : "");
                char *t_call_id = str_dup(tc->id ? tc->id : "");
                char *t_name = str_dup(tname);
                char *t_err_cat = NULL;
                if (result->error)
                {
                    t_err_cat = str_dup(result->error_category ? result->error_category : "execution_error");
                    char *err = NULL;
                    if (asprintf(&err, "Error: %s", result->error) < 0)
                        err = str_dup("Error");
                    free(t_content);
                    t_content = err;
                }
                if (!t_role || !t_content || !t_call_id || !t_name || (result->error && !t_err_cat))
                {
                    free(t_role); free(t_content); free(t_call_id); free(t_name); free(t_err_cat); free(tool_msg); tool_result_free(result); continue;
                }
                tool_msg->role = t_role;
                tool_msg->content = t_content;
                tool_msg->tool_call_id = t_call_id;
                tool_msg->tool_name = t_name;
                tool_msg->error_category = t_err_cat;
                if (msgs_count >= msgs_cap)
                {
                    msgs_cap *= 2;
                    Message *new_msgs = realloc(msgs, sizeof(Message) * msgs_cap);
                    if (!new_msgs) { free(t_role); free(t_content); free(t_call_id); free(t_name); free(t_err_cat); free(tool_msg); tool_result_free(result); break; }
                    msgs = new_msgs;
                }
                msgs[msgs_count++] = *tool_msg;
                free(tool_msg);
            }
            tool_result_free(result);
        }

        llm_response_free(resp);
    }

    for (int i = 0; i < msgs_count; i++)
    {
        free(msgs[i].role);
        free(msgs[i].content);
        free(msgs[i].id);
        free(msgs[i].tool_call_id);
        free(msgs[i].tool_name);
        free(msgs[i].error_category);
        free(msgs[i].thinking);
        if (msgs[i].tool_calls)
        {
            for (int j = 0; j < msgs[i].tool_calls_count; j++)
                tool_call_free(&msgs[i].tool_calls[j]);
            free(msgs[i].tool_calls);
        }
    }
    free(msgs);
    provider->destroy(provider);

    if (!final_content)
        return tool_result_create("(sub-agent produced no output)");

    ToolResult *tr = tool_result_create(final_content);
    free(final_content);
    return tr;
}

static void delegate_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

Tool *tool_delegate_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    DelegateToolCtx *ctx = calloc(1, sizeof(DelegateToolCtx));
    if (!ctx) { free(t); return NULL; }
    ctx->safety = safety;

    t->name = str_dup("delegate");
    t->description = str_dup("Delegate a task to a sub-agent that can use tools to complete it");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"task\":{\"type\":\"string\",\"description\":\"The task description for the sub-agent\"},"
        "\"iterations\":{\"type\":\"integer\",\"description\":\"Max iterations for sub-agent (default 10)\"}"
        "},\"required\":[\"task\"]}"
    );
    t->execute = delegate_execute;
    t->destroy = delegate_destroy;
    t->ctx = ctx;
    return t;
}
