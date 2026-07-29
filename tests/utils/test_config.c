#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include "config/config.h"

static void write_conf(const char *path, const char *content)
{
    FILE *fp = fopen(path, "w");
    ck_assert_ptr_nonnull(fp);
    fprintf(fp, "%s", content);
    fclose(fp);
}

START_TEST(test_conf_load_parses_sections_keys_and_comments)
{
    write_conf("/tmp/test_config.conf",
        "# comment\n"
        "key1 = value1\n"
        "\n"
        "[section]\n"
        "nested = nested_val\n");

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

START_TEST(test_conf_alloc_fail_mid)
{
    write_conf("/tmp/test_config_fail.conf",
        "alpha = first\n"
        "beta = second\n");

    config_test_set_alloc_fail(2);
    Conf *conf = conf_load("/tmp/test_config_fail.conf");
    ck_assert_ptr_nonnull(conf);

    const char *v = conf_get(conf, "alpha");
    ck_assert_ptr_null(v);

    v = conf_get(conf, "beta");
    ck_assert_ptr_nonnull(v);
    ck_assert_str_eq(v, "second");

    conf_free(conf);
    remove("/tmp/test_config_fail.conf");
}
END_TEST

START_TEST(test_conf_load_continuation_line)
{
    write_conf("/tmp/test_ctl.conf",
        "key1 = first line\n"
        " continuation\n");
    Conf *conf = conf_load("/tmp/test_ctl.conf");
    ck_assert_ptr_nonnull(conf);
    const char *v = conf_get(conf, "key1");
    ck_assert_ptr_nonnull(v);
    ck_assert_str_eq(v, "first line\ncontinuation");
    conf_free(conf);
    remove("/tmp/test_ctl.conf");
}
END_TEST

START_TEST(test_conf_get_int_invalid)
{
    write_conf("/tmp/test_ival.conf",
        "key = notanumber\n");
    Conf *conf = conf_load("/tmp/test_ival.conf");
    ck_assert_ptr_nonnull(conf);
    ck_assert_int_eq(conf_get_int(conf, "key", 99), 99);
    conf_free(conf);
    remove("/tmp/test_ival.conf");
}
END_TEST

Suite *config_suite(void)
{
    Suite *s = suite_create("Config");
    TCase *tc = tcase_create("Core");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_conf_load_parses_sections_keys_and_comments);
    tcase_add_test(tc, test_conf_load_nonexistent);
    tcase_add_test(tc, test_conf_alloc_fail_mid);
    tcase_add_test(tc, test_conf_load_continuation_line);
    tcase_add_test(tc, test_conf_get_int_invalid);
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