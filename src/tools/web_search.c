#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"

typedef struct {
    SafetyConfig *safety;
} SearchCtx;

static ToolResult *web_search_execute(Tool *self, const char *args_json)
{
    (void)self;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *query_json = cJSON_GetObjectItem(args, "query");
    if (!query_json || !cJSON_IsString(query_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'query' argument", "validation_error");
    }

    cJSON_Delete(args);

    return tool_result_create("Web search requires a search provider to be configured.");
}

static void web_search_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

Tool *tool_web_search_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    SearchCtx *ctx = calloc(1, sizeof(SearchCtx));
    if (!ctx) { free(t); return NULL; }
    ctx->safety = safety;
    (void)ctx;

    t->name = str_dup("web_search");
    t->description = str_dup("Search the web for information");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"query\":{\"type\":\"string\",\"description\":\"Search query\"}"
        "},\"required\":[\"query\"]}"
    );
    t->execute = web_search_execute;
    t->destroy = web_search_destroy;
    t->ctx = ctx;
    return t;
}
