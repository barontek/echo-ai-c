/*
 * test_deep_search.c - unit tests for the deep_search tool. The tool
 * delegates to the "web_search" and "web_fetch" registry tools, which are
 * stubbed here (no live network). Depends on: check, tool.h, registry.h.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>

#include "tools/tool.h"
#include "tools/registry.h"
#include "safety/safety.h"
#include "utils/string_utils.h"

Tool *tool_deep_search_create(SafetyConfig *safety);
void deep_search_test_set_alloc_fail(int nth_allocation);

static ToolResult *stub_search_execute(Tool *self, const char *args_json)
{
    (void)self;
    (void)args_json;
    return tool_result_create(
        "[{\"url\":\"https://example.com/a\"},{\"url\":\"https://example.com/b\"}]");
}

static ToolResult *stub_search_fail_execute(Tool *self, const char *args_json)
{
    (void)self;
    (void)args_json;
    return tool_result_error("stub search down", "execution_error");
}

static ToolResult *stub_fetch_execute(Tool *self, const char *args_json)
{
    (void)self;
    (void)args_json;
    return tool_result_create("page body text");
}

/* L2 regression source: a provider may return a JSON object (any valid
 * JSON) where the tool expects an array of results. */
static ToolResult *stub_search_object_execute(Tool *self, const char *args_json)
{
    (void)self;
    (void)args_json;
    return tool_result_create("{\"error\":\"provider returned object\"}");
}

static void stub_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self);
}

static Tool *make_stub(const char *name,
                       ToolResult *(*execute)(Tool *, const char *))
{
    Tool *t = calloc(1, sizeof(Tool));
    ck_assert_ptr_nonnull(t);
    t->name = str_dup(name);
    t->description = str_dup("test stub");
    t->parameters_schema = str_dup("{}");
    t->execute = execute;
    t->destroy = stub_destroy;
    return t;
}

/* Registers stubbed web_search/web_fetch and enables them so
 * registry_get() resolves them (registry owns them from here on). */
static void setup_stub_registry(void)
{
    registry_register(make_stub("web_search", stub_search_execute));
    registry_register(make_stub("web_fetch", stub_fetch_execute));
    registry_set_enabled("web_search, web_fetch");
}

START_TEST(test_deep_search_missing_query_is_validation_error)
{
    Tool *tool = tool_deep_search_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(tool, "{}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_nonnull(result->error);
    ck_assert_str_eq(result->error_category, "validation_error");
    tool_result_free(result);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_deep_search_search_tool_missing_reports_tool_not_found)
{
    Tool *tool = tool_deep_search_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(
        tool, "{\"query\":\"unit test query\"}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_nonnull(result->error);
    ck_assert_str_eq(result->error_category, "tool_not_found");
    tool_result_free(result);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_deep_search_combines_search_and_fetch_results)
{
    setup_stub_registry();
    Tool *tool = tool_deep_search_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(
        tool, "{\"query\":\"unit test query\"}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_null(result->error);
    cJSON *out = cJSON_Parse(result->content);
    ck_assert_ptr_nonnull(out);
    cJSON *query = cJSON_GetObjectItem(out, "query");
    ck_assert_ptr_nonnull(query);
    ck_assert_str_eq(query->valuestring, "unit test query");
    cJSON *fetched = cJSON_GetObjectItem(out, "fetched_pages");
    ck_assert_ptr_nonnull(fetched);
    ck_assert(cJSON_IsArray(fetched));
    /* both stub URLs fetched; each entry carries url + content_snippet */
    ck_assert_int_eq(cJSON_GetArraySize(fetched), 2);
    cJSON *first = cJSON_GetArrayItem(fetched, 0);
    ck_assert_ptr_nonnull(first);
    cJSON *snippet = cJSON_GetObjectItem(first, "content_snippet");
    ck_assert_ptr_nonnull(snippet);
    ck_assert_str_eq(snippet->valuestring, "page body text");
    cJSON *search_results = cJSON_GetObjectItem(out, "search_results");
    ck_assert_ptr_nonnull(search_results);
    ck_assert(cJSON_IsArray(search_results));
    ck_assert_int_eq(cJSON_GetArraySize(search_results), 2);
    cJSON_Delete(out);
    tool_result_free(result);
    tool->destroy(tool);
    registry_destroy();
}
END_TEST

START_TEST(test_deep_search_search_error_propagates)
{
    registry_register(make_stub("web_search", stub_search_fail_execute));
    registry_register(make_stub("web_fetch", stub_fetch_execute));
    registry_set_enabled("web_search, web_fetch");
    Tool *tool = tool_deep_search_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(
        tool, "{\"query\":\"will fail\"}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_nonnull(result->error);
    ck_assert_str_eq(result->error_category, "execution_error");
    tool_result_free(result);
    tool->destroy(tool);
    registry_destroy();
}
END_TEST

/* tool_deep_search_create allocates: calloc(t), 3x str_dup; a partial
 * tool must never be returned (NULL with nothing leaked). */
START_TEST(test_deep_search_create_allocation_failure_returns_null)
{
    for (int fail_at = 1; fail_at <= 3; fail_at++)
    {
        deep_search_test_set_alloc_fail(fail_at);
        Tool *tool = tool_deep_search_create(NULL);
        ck_assert_ptr_null(tool);
    }
    deep_search_test_set_alloc_fail(-1);
}
END_TEST

/* The execute path allocates query (1) then the search args (2); each
 * failure must return an error result with no partial state. The per-URL
 * fetch_args asprintf (3+) is best-effort and must degrade to a result
 * that still parses. */
START_TEST(test_deep_search_allocation_failure_returns_error)
{
    setup_stub_registry();
    Tool *tool = tool_deep_search_create(NULL);
    ck_assert_ptr_nonnull(tool);
    for (int fail_at = 1; fail_at <= 2; fail_at++)
    {
        deep_search_test_set_alloc_fail(fail_at);
        ToolResult *result = tool->execute(
            tool, "{\"query\":\"fault query\"}");
        ck_assert_ptr_nonnull(result);
        ck_assert_ptr_nonnull(result->error);
        tool_result_free(result);
    }
    deep_search_test_set_alloc_fail(-1);
    tool->destroy(tool);
    registry_destroy();
}
END_TEST

/* L2 regression: a search provider returning a JSON object (not an array)
 * used to be freed twice — once by cJSON_Delete(output) after the transfer
 * and again by the inverted guard — an ASan double-free/use-after-free.
 * The object must be consumed exactly once and surfaced as the "none"
 * marker instead. */
START_TEST(test_deep_search_non_array_search_result_is_handled)
{
    registry_register(make_stub("web_search", stub_search_object_execute));
    registry_register(make_stub("web_fetch", stub_fetch_execute));
    registry_set_enabled("web_search, web_fetch");
    Tool *tool = tool_deep_search_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *result = tool->execute(tool, "{\"query\":\"object shape\"}");
    ck_assert_ptr_nonnull(result);
    ck_assert_ptr_null(result->error);
    cJSON *out = cJSON_Parse(result->content);
    ck_assert_ptr_nonnull(out);
    cJSON *search_results = cJSON_GetObjectItem(out, "search_results");
    ck_assert_ptr_nonnull(search_results);
    ck_assert(cJSON_IsString(search_results));
    ck_assert_str_eq(search_results->valuestring, "none");
    cJSON_Delete(out);
    tool_result_free(result);
    tool->destroy(tool);
    registry_destroy();
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("DeepSearch");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_deep_search_missing_query_is_validation_error);
    tcase_add_test(tc, test_deep_search_search_tool_missing_reports_tool_not_found);
    tcase_add_test(tc, test_deep_search_combines_search_and_fetch_results);
    tcase_add_test(tc, test_deep_search_search_error_propagates);
    tcase_add_test(tc, test_deep_search_create_allocation_failure_returns_null);
    tcase_add_test(tc, test_deep_search_allocation_failure_returns_error);
    tcase_add_test(tc, test_deep_search_non_array_search_result_is_handled);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
