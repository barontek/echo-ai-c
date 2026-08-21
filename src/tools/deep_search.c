/*
 * deep_search.c - multi-step research tool: searches the web through the
 * web_search tool, fetches the top pages with web_fetch, and returns the
 * combined findings as JSON. Depends on: tool.h, registry, safety,
 * string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DEEP_SEARCH_TEST
#include <stdarg.h>
#endif

#include "tool.h"
#include "registry.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

#ifdef DEEP_SEARCH_TEST
/* Test-only allocator fault injection: shared counter across str_dup,
 * asprintf and malloc so tests can fail the Nth allocation inside
 * deep_search_execute. Only the test target defines DEEP_SEARCH_TEST. */
static int deep_search_test_alloc_counter = 0;
static int deep_search_test_alloc_fail_at = -1;

void deep_search_test_set_alloc_fail(int nth_allocation)
{
    deep_search_test_alloc_counter = 0;
    deep_search_test_alloc_fail_at = nth_allocation;
}

static char *deep_search_test_strdup(const char *s)
{
    deep_search_test_alloc_counter++;
    if (deep_search_test_alloc_counter == deep_search_test_alloc_fail_at)
        return NULL;
    return str_dup(s);
}

static int deep_search_test_asprintf(char **strp, const char *fmt, ...)
{
    deep_search_test_alloc_counter++;
    if (deep_search_test_alloc_counter == deep_search_test_alloc_fail_at)
    {
        *strp = NULL;
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int rc = vasprintf(strp, fmt, ap);
    va_end(ap);
    return rc;
}

static void *deep_search_test_malloc(size_t size)
{
    deep_search_test_alloc_counter++;
    if (deep_search_test_alloc_counter == deep_search_test_alloc_fail_at)
        return NULL;
    return malloc(size);
}

#define str_dup deep_search_test_strdup
#define asprintf deep_search_test_asprintf
#define malloc deep_search_test_malloc
#endif

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

    char *query = str_dup(cJSON_GetStringValue(query_json));
    if (!query)
    {
        cJSON_Delete(args);
        return tool_result_error("oom", "execution_error");
    }
    cJSON_Delete(args);

    /* Step 1: search the web */
    Tool *web_search_tool = registry_get("web_search");
    if (!web_search_tool)
    {
        free(query);
        return tool_result_error("web_search tool not available", "tool_not_found");
    }

    char *search_args = NULL;
    if (asprintf(&search_args, "{\"query\":\"%s\",\"num_results\":5}", query) < 0)
    {
        free(query);
        return tool_result_error("oom", "execution_error");
    }

    ToolResult *search_result = web_search_tool->execute(web_search_tool, search_args);
    free(search_args);

    if (!search_result || search_result->error)
    {
        ToolResult *err = tool_result_error(search_result ? search_result->error : "search failed",
                                            search_result ? search_result->error_category : "execution_error");
        if (search_result) tool_result_free(search_result);
        free(query);
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
                      if (fr->content) memcpy(snippet, fr->content, snippet_len);
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

    /* Step 3: build combined result. Ownership of results_json transfers
     * into output only when it is an array; any other shape is discarded
     * here so the free at cJSON_Delete(output) is the ONLY free of it
     * (the historical bug freed it again afterwards). */
    cJSON *output = cJSON_CreateObject();
    cJSON_AddStringToObject(output, "query", query);
    free(query);
    if (results_json)
    {
        if (cJSON_IsArray(results_json))
            cJSON_AddItemToObject(output, "search_results", results_json);
        else
        {
            cJSON_Delete(results_json);
            cJSON_AddStringToObject(output, "search_results", "none");
        }
    }
    else
    {
        cJSON_AddStringToObject(output, "search_results", "none");
    }
    cJSON_AddItemToObject(output, "fetched_pages", fetched_arr);

    char *result = cJSON_PrintUnformatted(output);
    cJSON_Delete(output);

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

/**
 * tool_deep_search_create - construct the deep_search tool
 * @safety: accepted for interface uniformity only; ignored by this tool
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy().
 */
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
    if (!t->name || !t->description || !t->parameters_schema)
    {
        free(t->name);
        free(t->description);
        free(t->parameters_schema);
        free(t);
        return NULL;
    }
    t->execute = deep_search_execute;
    t->destroy = deep_search_destroy;
    t->ctx = NULL;
    return t;
}
