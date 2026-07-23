#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include "config/config.h"

START_TEST(test_conf_load_and_get)
{
    FILE *fp = fopen("/tmp/test_config.conf", "w");
    ck_assert_ptr_nonnull(fp);
    fprintf(fp, "# comment\n");
    fprintf(fp, "key1 = value1\n");
    fprintf(fp, "\n");
    fprintf(fp, "[section]\n");
    fprintf(fp, "nested = nested_val\n");
    fclose(fp);

    Conf *conf = conf_load("/tmp/test_config.conf");
    ck_assert_ptr_nonnull(conf);

    const char *v = conf_get(conf, "key1");
    ck_assert_ptr_nonnull(v);
    ck_assert_str_eq(v, "value1");

    v = conf_get(conf, "section.nested");
    ck_assert_ptr_nonnull(v);
    ck_assert_str_eq(v, "nested_val");

    v = conf_get(conf, "nonexistent");
    ck_assert_ptr_null(v);

    int iv = conf_get_int(conf, "section.nested", 42);
    ck_assert_int_eq(iv, 42);

    conf_free(conf);
    remove("/tmp/test_config.conf");
}
END_TEST

START_TEST(test_conf_load_nonexistent)
{
    Conf *conf = conf_load("/tmp/nonexistent.conf");
    ck_assert_ptr_null(conf);
}
END_TEST

Suite *config_suite(void)
{
    Suite *s = suite_create("Config");
    TCase *tc = tcase_create("Core");
    tcase_add_test(tc, test_conf_load_and_get);
    tcase_add_test(tc, test_conf_load_nonexistent);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = config_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
