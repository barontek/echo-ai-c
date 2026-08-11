#include <check.h>
#include <stdlib.h>
#include <string.h>

#include "../src/tools/registry.h"
#include "../src/utils/string_utils.h"

/* test_registry - registry unit tests. Depends on: check, the module under test. */
static void fake_tool_destroy(Tool *tool)
{
    free(tool->name);
    free(tool);
}

static Tool *fake_tool_create(const char *name)
{
    Tool *tool = calloc(1, sizeof(Tool));
    ck_assert_ptr_nonnull(tool);
    tool->name = str_dup(name);
    ck_assert_ptr_nonnull(tool->name);
    tool->destroy = fake_tool_destroy;
    return tool;
}

START_TEST(test_registry_rejects_disabled_tool_lookup)
{
    registry_register(fake_tool_create("disabled"));
    ck_assert_ptr_null(registry_get("disabled"));
    ck_assert_ptr_null(registry_get(NULL));
    registry_destroy();
}
END_TEST

START_TEST(test_registry_returns_enabled_tool)
{
    registry_register(fake_tool_create("enabled"));
    registry_set_enabled("enabled");
    ck_assert_ptr_nonnull(registry_get("enabled"));
    registry_destroy();
}
END_TEST

START_TEST(test_registry_set_delegate_config_normal)
{
    ck_assert_int_ne(registry_get_delegate_config(NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL), 0);

    registry_set_delegate_config("test_provider", "http://test_url", "sk-test",
        "test_model", 1024, 300, 0.7, 30, 10);

    const char *pn = NULL, *bu = NULL, *tk = NULL, *md = NULL;
    int ret = registry_get_delegate_config(&pn, &bu, &tk, &md,
                                           NULL, NULL, NULL, NULL, NULL);
    ck_assert_int_eq(ret, 0);
    ck_assert_str_eq(pn, "test_provider");
    ck_assert_str_eq(bu, "http://test_url");
    ck_assert_str_eq(tk, "sk-test");
    ck_assert_str_eq(md, "test_model");

    registry_set_delegate_config(NULL, NULL, NULL, NULL, 0, 0, 0.0, 0, 0);
    ck_assert_int_ne(registry_get_delegate_config(NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL), 0);
}
END_TEST

START_TEST(test_registry_set_delegate_config_fail_first_alloc)
{
    ck_assert_int_ne(registry_get_delegate_config(NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL), 0);

    registry_test_set_alloc_fail(1);
    registry_set_delegate_config("provider_a", "http://url_a", "token_a",
        "model_a", 512, 60, 0.5, 15, 5);

    ck_assert_int_ne(registry_get_delegate_config(NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL), 0);
}
END_TEST

START_TEST(test_registry_set_delegate_config_fail_second_alloc)
{
    registry_set_delegate_config("first", "http://first", "token_first",
        "first_model", 1024, 300, 0.7, 30, 10);

    registry_test_set_alloc_fail(2);
    registry_set_delegate_config("second", "http://second", "token_second",
        "second_model", 2048, 600, 0.3, 60, 20);

    const char *pn = NULL, *bu = NULL, *md = NULL;
    int ret = registry_get_delegate_config(&pn, &bu, NULL, &md,
                                           NULL, NULL, NULL, NULL, NULL);
    ck_assert_int_eq(ret, 0);
    ck_assert_str_eq(pn, "first");
    ck_assert_str_eq(bu, "http://first");
    ck_assert_str_eq(md, "first_model");

    registry_set_delegate_config(NULL, NULL, NULL, NULL, 0, 0, 0.0, 0, 0);
}
END_TEST

START_TEST(test_registry_set_delegate_config_fail_third_alloc)
{
    ck_assert_int_ne(registry_get_delegate_config(NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL), 0);

    registry_test_set_alloc_fail(3);
    registry_set_delegate_config("provider_c", "http://url_c", "token_c",
        "model_c", 1024, 300, 0.7, 30, 10);

    ck_assert_int_ne(registry_get_delegate_config(NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL), 0);
}
END_TEST

/* B1 regression: registry_set_delegate_config str_dups four strings into
 * the registry-owned delegate_config; registry_destroy used to leak them
 * on every shutdown. With the fix, destroy releases exactly those four
 * allocations (no tools registered, so nothing else frees in registry.c). */
START_TEST(test_registry_destroy_releases_delegate_config)
{
    registry_set_delegate_config("provider_x", "http://url_x", "token_x",
                                 "model_x", 1024, 300, 0.7, 30, 10);
    /* Tally reset after the set, so only the destroy's frees count */
    registry_test_free_reset();
    registry_destroy();
    ck_assert_int_eq(registry_test_free_tally(), 4);
    /* registry is reusable after destroy */
    registry_set_delegate_config("provider_y", "http://url_y", "token_y",
                                 "model_y", 1024, 300, 0.7, 30, 10);
    const char *pn = NULL, *bu = NULL, *tk = NULL, *md = NULL;
    ck_assert_int_eq(registry_get_delegate_config(&pn, &bu, &tk, &md,
        NULL, NULL, NULL, NULL, NULL), 0);
    ck_assert_str_eq(pn, "provider_y");
    registry_set_delegate_config(NULL, NULL, NULL, NULL, 0, 0, 0.0, 0, 0);
    registry_test_free_reset();
    registry_destroy();
    /* the four NULL fields still go through free (NULL-safe) */
    ck_assert_int_eq(registry_test_free_tally(), 4);
}
END_TEST

Suite *registry_suite(void)
{
    Suite *s = suite_create("Registry");

    TCase *tc_normal = tcase_create("Normal");
    tcase_add_test(tc_normal, test_registry_rejects_disabled_tool_lookup);
    tcase_add_test(tc_normal, test_registry_returns_enabled_tool);
    tcase_add_test(tc_normal, test_registry_set_delegate_config_normal);
    suite_add_tcase(s, tc_normal);

    TCase *tc_fault = tcase_create("FaultInjection");
    tcase_add_test(tc_fault, test_registry_set_delegate_config_fail_first_alloc);
    tcase_add_test(tc_fault, test_registry_set_delegate_config_fail_second_alloc);
    tcase_add_test(tc_fault, test_registry_set_delegate_config_fail_third_alloc);
    tcase_add_test(tc_fault, test_registry_destroy_releases_delegate_config);
    suite_add_tcase(s, tc_fault);

    return s;
}

int main(void)
{
    Suite *s = registry_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failures = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failures ? 1 : 0;
}
