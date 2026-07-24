#include <check.h>
#include <stdlib.h>
#include <string.h>

#include "../src/tools/registry.h"

START_TEST(test_registry_set_delegate_config_normal)
{
    ck_assert_int_ne(registry_get_delegate_config(NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL), 0);

    registry_set_delegate_config("test_provider", "http://test_url", "test_model",
        1024, 300, 0.7, 30, 10);

    const char *pn = NULL, *bu = NULL, *md = NULL;
    int ret = registry_get_delegate_config(&pn, &bu, &md, NULL, NULL, NULL, NULL, NULL);
    ck_assert_int_eq(ret, 0);
    ck_assert_str_eq(pn, "test_provider");
    ck_assert_str_eq(bu, "http://test_url");
    ck_assert_str_eq(md, "test_model");

    registry_set_delegate_config(NULL, NULL, NULL, 0, 0, 0.0, 0, 0);
    ck_assert_int_ne(registry_get_delegate_config(NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL), 0);
}
END_TEST

START_TEST(test_registry_set_delegate_config_fail_first_alloc)
{
    ck_assert_int_ne(registry_get_delegate_config(NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL), 0);

    registry_test_set_alloc_fail(1);
    registry_set_delegate_config("provider_a", "http://url_a", "model_a",
        512, 60, 0.5, 15, 5);

    ck_assert_int_ne(registry_get_delegate_config(NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL), 0);
}
END_TEST

START_TEST(test_registry_set_delegate_config_fail_second_alloc)
{
    registry_set_delegate_config("first", "http://first", "first_model",
        1024, 300, 0.7, 30, 10);

    registry_test_set_alloc_fail(2);
    registry_set_delegate_config("second", "http://second", "second_model",
        2048, 600, 0.3, 60, 20);

    const char *pn = NULL, *bu = NULL, *md = NULL;
    int ret = registry_get_delegate_config(&pn, &bu, &md, NULL, NULL, NULL, NULL, NULL);
    ck_assert_int_eq(ret, 0);
    ck_assert_str_eq(pn, "first");
    ck_assert_str_eq(bu, "http://first");
    ck_assert_str_eq(md, "first_model");

    registry_set_delegate_config(NULL, NULL, NULL, 0, 0, 0.0, 0, 0);
}
END_TEST

START_TEST(test_registry_set_delegate_config_fail_third_alloc)
{
    ck_assert_int_ne(registry_get_delegate_config(NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL), 0);

    registry_test_set_alloc_fail(3);
    registry_set_delegate_config("provider_c", "http://url_c", "model_c",
        1024, 300, 0.7, 30, 10);

    ck_assert_int_ne(registry_get_delegate_config(NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL), 0);
}
END_TEST

Suite *registry_suite(void)
{
    Suite *s = suite_create("Registry");
    TCase *tc = tcase_create("Core");

    tcase_add_test(tc, test_registry_set_delegate_config_normal);
    tcase_add_test(tc, test_registry_set_delegate_config_fail_first_alloc);
    tcase_add_test(tc, test_registry_set_delegate_config_fail_second_alloc);
    tcase_add_test(tc, test_registry_set_delegate_config_fail_third_alloc);
    suite_add_tcase(s, tc);

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
