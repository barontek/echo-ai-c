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

    /* Optional numbered options: appended to the question as "1. opt"
     * lines; a bare integer answer resolves to the matching option. */
    cJSON *opts = cJSON_GetObjectItem(args, "options");
    int opt_count = opts && cJSON_IsArray(opts) ? cJSON_GetArraySize(opts) : 0;
    char **opt_text = NULL;
    if (opt_count > 0)
    {
        opt_text = calloc((size_t)opt_count, sizeof(char *));
        if (!opt_text)
        {
            cJSON_Delete(args);
            return tool_result_error("out of memory", "execution_error");
        }
        cJSON *item;
        int k = 0;
        cJSON_ArrayForEach(item, opts)
        {
            const char *s = cJSON_GetStringValue(item);
            if (s && k < opt_count)
                opt_text[k++] = str_dup(s);
        }
        opt_count = k;
    }

    char *q = str_dup(cJSON_GetStringValue(question));
    cJSON_Delete(args);
    if (!q)
    {
        for (int i = 0; i < opt_count; i++) free(opt_text[i]);
        free(opt_text);
        return tool_result_error("out of memory", "execution_error");
    }
    for (int i = 0; i < opt_count; i++)
    {
        char *nb = NULL;
        if (asprintf(&nb, "%s\n%d. %s", q, i + 1,
                     opt_text[i] ? opt_text[i] : "") >= 0)
        {
            free(q);
            q = nb;
        }
    }

    char *answer = NULL;
    if (registry_has_ask_user_callback())
    {
        answer = registry_invoke_ask_user(q);
        if (!answer)
        {
            free(q);
            for (int i = 0; i < opt_count; i++) free(opt_text[i]);
            free(opt_text);
            return tool_result_error("question cancelled", "cancelled");
        }
    }
    else
    {
        fprintf(stderr, "\n[Ask User] %s\n> ", q); // NOLINT(cert-err33-c)
        fflush(stderr); // NOLINT(cert-err33-c)

        size_t cap = 0;
        ssize_t len = getline(&answer, &cap, stdin);
        if (len < 0)
        {
            /* C11: EIO/closed stdin used to masquerade as "the user did
             * not respond" — a successful result. Report it as an error
             * instead. */
            free(answer);
            free(q);
            for (int i = 0; i < opt_count; i++) free(opt_text[i]);
            free(opt_text);
            return tool_result_error("failed reading user input",
                                     "execution_error");
        }
        if (len > 0 && answer[len - 1] == '\n')
            answer[len - 1] = '\0';
    }

    /* Resolve a bare option number to its text */
    if (opt_count > 0 && answer)
    {
        char *end = NULL;
        long n = strtol(answer, &end, 10);
        if (end && *end == '\0' && n >= 1 && n <= opt_count && opt_text[n - 1]) // NOLINT(clang-analyzer-security.ArrayBound)
        {
            char *resolved = str_dup(opt_text[n - 1]);
            if (resolved)
            {
                free(answer);
                answer = resolved;
            }
        }
    }

    free(q);
    for (int i = 0; i < opt_count; i++) free(opt_text[i]);
    free(opt_text);

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
        "\"question\":{\"type\":\"string\",\"description\":\"Question to ask the user\"},"
        "\"options\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
        "\"description\":\"Optional numbered choices; the user may answer with a number\"}"
        "},\"required\":[\"question\"]}"
    );
    t->execute = ask_user_execute;
    t->destroy = ask_user_destroy;
    t->ctx = ctx;
    return t;
}
