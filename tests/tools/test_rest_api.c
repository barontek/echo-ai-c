/*
 * test_rest_api.c - unit tests for the rest_api tool's validation and
 * network-policy paths (no live HTTP). Depends on: check, tool.h,
 * safety, config.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "tools/tool.h"
#include "safety/safety.h"

Tool *tool_rest_api_create(SafetyConfig *safety);

START_TEST(test_rest_api_missing_url_is_validation_error)
{
    SafetyConfig *safety = safety_config_create();
    ck_assert_ptr_nonnull(safety);
    Tool *tool = tool_rest_api_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"method\":\"GET\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);
    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

/* With allow_network off every URL is rejected before any socket work
 * happens (no live HTTP in this suite). */
START_TEST(test_rest_api_url_rejected_when_network_disabled)
{
    SafetyConfig *safety = safety_config_create();
    ck_assert_ptr_nonnull(safety);
    safety->allow_network = 0;
    Tool *tool = tool_rest_api_create(safety);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"url\":\"https://example.com\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "policy_denied");
    tool_result_free(r);
    tool->destroy(tool);
    safety_config_free(safety);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("RestApi");
    TCase *tc = tcase_create("Policy");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_rest_api_missing_url_is_validation_error);
    tcase_add_test(tc, test_rest_api_url_rejected_when_network_disabled);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
