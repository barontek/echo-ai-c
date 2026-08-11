/*
 * tool_ask_user.c - interactive prompt tool: asks the user a question
 * through the registry's ask-user callback when one is registered,
 * otherwise falls back to reading a line from stdin. Depends on: tool.h,
 * registry, string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "tool.h"
#include "registry.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
} AskUserCtx;

static ToolResult *ask_user_execute(Tool *self, const char *args_json)
{
    (void)self;
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *question = cJSON_GetObjectItem(args, "question");
    if (!question || !cJSON_IsString(question))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'question' argument", "validation_error");
    }

    char *q = str_dup(cJSON_GetStringValue(question));
    cJSON_Delete(args);

    char *answer = NULL;
    if (registry_has_ask_user_callback())
    {
        answer = registry_invoke_ask_user(q);
        if (!answer)
        {
            free(q);
            return tool_result_error("question cancelled", "cancelled");
        }
    }
    else
    {
        fprintf(stderr, "\n[Ask User] %s\n> ", q);
        fflush(stderr);

        size_t cap = 0;
        ssize_t len = getline(&answer, &cap, stdin);
        if (len < 0)
        {
            /* C11: EIO/closed stdin used to masquerade as "the user did
             * not respond" — a successful result. Report it as an error
             * instead. */
            free(answer);
            free(q);
            return tool_result_error("failed reading user input",
                                     "execution_error");
        }
        if (len > 0 && answer[len - 1] == '\n')
            answer[len - 1] = '\0';
    }

    free(q);

    ToolResult *tr = tool_result_create(answer ? answer : "(user did not respond)");
    free(answer);
    return tr;
}

static void ask_user_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_ask_user_create - construct the ask_user tool
 * @safety: borrowed SafetyConfig; retained in the tool's context but not
 * consulted by the execute path
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
Tool *tool_ask_user_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    AskUserCtx *ctx = calloc(1, sizeof(AskUserCtx));
    if (!ctx) {
        free(t);
        return NULL;
    }
    ctx->safety = safety;

    t->name = str_dup("ask_user");
    t->description = str_dup("Ask the user a question and wait for their response");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"question\":{\"type\":\"string\",\"description\":\"Question to ask the user\"}"
        "},\"required\":[\"question\"]}"
    );
    t->execute = ask_user_execute;
    t->destroy = ask_user_destroy;
    t->ctx = ctx;
    return t;
}
