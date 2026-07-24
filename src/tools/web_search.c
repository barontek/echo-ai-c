#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool.h"
#include "registry.h"
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

    const char *query = cJSON_GetStringValue(query_json);

    cJSON *num_json = cJSON_GetObjectItem(args, "num_results");
    int num_results = num_json && cJSON_IsNumber(num_json) ? num_json->valueint : 5;
    cJSON_Delete(args);

    SearchProvider *sp = registry_get_search_provider();
    if (!sp)
        return tool_result_create("Web search requires a search provider to be configured.\n"
                                  "Set [search] provider and api_key in config.conf");

    char *raw = sp->search(sp, query, num_results);
    if (!raw)
        return tool_result_error("search returned no result", "execution_error");

    ToolResult *tr = tool_result_create(raw);
    free(raw);
    return tr;
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

    t->name = str_dup("web_search");
    t->description = str_dup("Search the web for information using Brave, DuckDuckGo, or Tavily");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"query\":{\"type\":\"string\",\"description\":\"Search query\"},"
        "\"num_results\":{\"type\":\"integer\",\"description\":\"Number of results (default 5)\"}"
        "},\"required\":[\"query\"]}"
    );
    t->execute = web_search_execute;
    t->destroy = web_search_destroy;
    t->ctx = ctx;
    return t;
}
