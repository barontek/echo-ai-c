#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool.h"
#include "registry.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

static ToolResult *deep_search_execute(Tool *self, const char *args_json)
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
    cJSON_Delete(args);

    /* Step 1: search the web */
    Tool *web_search_tool = registry_get("web_search");
    if (!web_search_tool)
        return tool_result_error("web_search tool not available", "tool_not_found");

    char *search_args = NULL;
    if (asprintf(&search_args, "{\"query\":\"%s\",\"num_results\":5}", query) < 0)
        return tool_result_error("oom", "execution_error");

    ToolResult *search_result = web_search_tool->execute(web_search_tool, search_args);
    free(search_args);

    if (!search_result || search_result->error)
    {
        ToolResult *err = tool_result_error(search_result ? search_result->error : "search failed",
                                            search_result ? search_result->error_category : "execution_error");
        if (search_result) tool_result_free(search_result);
        return err;
    }

    /* Step 2: parse results and fetch top pages */
    cJSON *results_json = cJSON_Parse(search_result->content);
    tool_result_free(search_result);

    cJSON *urls_to_fetch = NULL;
    if (results_json && cJSON_IsArray(results_json))
    {
        urls_to_fetch = cJSON_CreateArray();
        int count = cJSON_GetArraySize(results_json);
        for (int i = 0; i < count && i < 3; i++)
        {
            cJSON *item = cJSON_GetArrayItem(results_json, i);
            cJSON *url_item = cJSON_GetObjectItem(item, "url");
            if (url_item && url_item->valuestring)
                cJSON_AddItemToArray(urls_to_fetch, cJSON_CreateString(url_item->valuestring));
        }
    }

    Tool *fetch_tool = registry_get("web_fetch");
    cJSON *fetched_arr = cJSON_CreateArray();

    if (urls_to_fetch && fetch_tool)
    {
        int url_count = cJSON_GetArraySize(urls_to_fetch);
        for (int i = 0; i < url_count; i++)
        {
            cJSON *url_item = cJSON_GetArrayItem(urls_to_fetch, i);
            if (!url_item || !url_item->valuestring) continue;

            char *fetch_args = NULL;
            if (asprintf(&fetch_args, "{\"url\":\"%s\"}", url_item->valuestring) < 0)
                continue;

            ToolResult *fr = fetch_tool->execute(fetch_tool, fetch_args);
            free(fetch_args);

            if (fr && !fr->error)
            {
                cJSON *fe = cJSON_CreateObject();
                cJSON_AddStringToObject(fe, "url", url_item->valuestring);
                size_t clen = fr->content ? strlen(fr->content) : 0;
                size_t snippet_len = clen < 2000 ? clen : 2000;
                char *snippet = malloc(snippet_len + 1);
                if (snippet)
                {
                    memcpy(snippet, fr->content, snippet_len);
                    snippet[snippet_len] = '\0';
                    cJSON_AddStringToObject(fe, "content_snippet", snippet);
                    free(snippet);
                }
                cJSON_AddItemToArray(fetched_arr, fe);
            }
            if (fr) tool_result_free(fr);
        }
    }

    if (urls_to_fetch) cJSON_Delete(urls_to_fetch);

    /* Step 3: build combined result */
    cJSON *output = cJSON_CreateObject();
    cJSON_AddStringToObject(output, "query", query);
    if (results_json)
        cJSON_AddItemToObject(output, "search_results", results_json);
    else
        cJSON_AddStringToObject(output, "search_results", "none");
    cJSON_AddItemToObject(output, "fetched_pages", fetched_arr);

    char *result = cJSON_PrintUnformatted(output);
    cJSON_Delete(output);
    if (results_json && !cJSON_IsArray(results_json)) /* already added */
        cJSON_Delete(results_json);

    if (!result) return tool_result_create("(no output)");
    ToolResult *tr = tool_result_create(result);
    free(result);
    return tr;
}

static void deep_search_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self);
}

Tool *tool_deep_search_create(SafetyConfig *safety)
{
    (void)safety;
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    t->name = str_dup("deep_search");
    t->description = str_dup("Multi-step research: search the web, fetch top results, and return combined findings");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"query\":{\"type\":\"string\",\"description\":\"Research query\"}"
        "},\"required\":[\"query\"]}"
    );
    t->execute = deep_search_execute;
    t->destroy = deep_search_destroy;
    t->ctx = NULL;
    return t;
}
