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

    const char *q = cJSON_GetStringValue(question);
    cJSON_Delete(args);

    char *answer = registry_invoke_ask_user(q);
    if (!answer)
    {
        fprintf(stderr, "\n[Ask User] %s\n> ", q);
        fflush(stderr);

        size_t cap = 0;
        ssize_t len = getline(&answer, &cap, stdin);
        if (len < 0)
        {
            free(answer);
            return tool_result_create("(user did not respond)");
        }
        if (len > 0 && answer[len - 1] == '\n')
            answer[len - 1] = '\0';
    }

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

Tool *tool_ask_user_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    AskUserCtx *ctx = calloc(1, sizeof(AskUserCtx));
    if (!ctx) { free(t); return NULL; }
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
