/*
 * test_web_search.c - unit tests for the web_search tool with a stubbed
 * SearchProvider (no live network). Depends on: check, tool.h, registry,
 * search_provider, safety.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "tools/tool.h"
#include "tools/registry.h"
#include "tools/search_provider.h"

Tool *tool_web_search_create(SafetyConfig *safety);

static char *stub_search(SearchProvider *self, const char *query,
                         int num_results)
{
    (void)self;
    char *out = NULL;
    if (asprintf(&out, "[{\"title\":\"t\",\"url\":\"u\",\"snippet\":\"%s %d\"}]",
                 query, num_results) < 0)
        return NULL;
    return out;
}

static SearchProvider stub_provider = {
    .name = "stub",
    .search = stub_search,
    .destroy = NULL,
    .ctx = NULL,
};

static void install_provider(void)
{
    registry_set_search_provider(&stub_provider);
}

START_TEST(test_web_search_invokes_provider_and_returns_raw)
{
    install_provider();
    Tool *tool = tool_web_search_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(
        tool, "{\"query\":\"hello\",\"num_results\":3}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert(strstr(r->content, "\"snippet\":\"hello 3\"") != NULL);
    tool_result_free(r);
    tool->destroy(tool);
    registry_set_search_provider(NULL);
}
END_TEST

START_TEST(test_web_search_no_provider_returns_hint)
{
    registry_set_search_provider(NULL);
    Tool *tool = tool_web_search_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"query\":\"hello\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert(strstr(r->content, "requires a search provider") != NULL);
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

START_TEST(test_web_search_missing_query_is_validation_error)
{
    install_provider();
    Tool *tool = tool_web_search_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);
    tool->destroy(tool);
    registry_set_search_provider(NULL);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("WebSearch");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_web_search_invokes_provider_and_returns_raw);
    tcase_add_test(tc, test_web_search_no_provider_returns_hint);
    tcase_add_test(tc, test_web_search_missing_query_is_validation_error);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
