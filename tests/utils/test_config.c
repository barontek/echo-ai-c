#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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
    ck_assert_int_eq(remove("/tmp/test_config.conf"), 0);
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
    ck_assert_int_eq(remove("/tmp/test_config_fail.conf"), 0);
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
    ck_assert_int_eq(remove("/tmp/test_ctl.conf"), 0);
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
    ck_assert_int_eq(remove("/tmp/test_ival.conf"), 0);
}
END_TEST

START_TEST(test_conf_provider_tokens_all)
{
    write_conf("/tmp/test_prov.conf",
        "[providers]\n"
        "openai = sk-openai-1\n"
        "anthropic = sk-ant-2\n");
    Conf *conf = conf_load("/tmp/test_prov.conf");
    ck_assert_ptr_nonnull(conf);

    ConfToken *tokens = NULL;
    int count = 0;
    int rc = conf_provider_tokens_alloc(conf, &tokens, &count);
    ck_assert_int_eq(rc, 0);
    ck_assert_int_eq(count, 2);

    const char *openai_token = NULL;
    const char *anthropic_token = NULL;
    for (int i = 0; i < count; i++)
    {
        if (strcmp(tokens[i].provider, "openai") == 0)
            openai_token = tokens[i].token;
        else if (strcmp(tokens[i].provider, "anthropic") == 0)
            anthropic_token = tokens[i].token;
    }
    ck_assert_ptr_nonnull(openai_token);
    ck_assert_str_eq(openai_token, "sk-openai-1");
    ck_assert_ptr_nonnull(anthropic_token);
    ck_assert_str_eq(anthropic_token, "sk-ant-2");

    conf_token_list_free(tokens, count);
    conf_free(conf);
    ck_assert_int_eq(remove("/tmp/test_prov.conf"), 0);
}
END_TEST

START_TEST(test_conf_provider_tokens_empty)
{
    write_conf("/tmp/test_prov_empty.conf",
        "[agent]\n"
        "model = gemma2:2b\n");
    Conf *conf = conf_load("/tmp/test_prov_empty.conf");
    ck_assert_ptr_nonnull(conf);

    ConfToken *tokens = NULL;
    int count = 0;
    int rc = conf_provider_tokens_alloc(conf, &tokens, &count);
    ck_assert_int_eq(rc, 0);
    ck_assert_int_eq(count, 0);
    ck_assert_ptr_null(tokens);

    conf_token_list_free(tokens, count);
    conf_free(conf);
    ck_assert_int_eq(remove("/tmp/test_prov_empty.conf"), 0);
}
END_TEST

START_TEST(test_conf_provider_tokens_alloc_fail)
{
    write_conf("/tmp/test_prov_fail.conf",
        "[providers]\n"
        "openai = sk-openai-1\n"
        "anthropic = sk-ant-2\n");
    Conf *conf = conf_load("/tmp/test_prov_fail.conf");
    ck_assert_ptr_nonnull(conf);

    /* Fail the first str_dup (provider name of the first entry):
     * nothing must be returned and nothing leaked. */
    config_test_set_alloc_fail(1);
    ConfToken *tokens = NULL;
    int count = 0;
    int rc = conf_provider_tokens_alloc(conf, &tokens, &count);
    ck_assert_int_eq(rc, -1);
    ck_assert_ptr_null(tokens);
    ck_assert_int_eq(count, 0);

    conf_free(conf);
    ck_assert_int_eq(remove("/tmp/test_prov_fail.conf"), 0);
}
END_TEST

START_TEST(test_conf_provider_token_opencode_zen_aliases_opencode)
{
    write_conf("/tmp/test_prov_token.conf",
        "[providers]\n"
        "opencode = sk-zen-1\n"
        "openai = sk-openai-2\n");
    Conf *conf = conf_load("/tmp/test_prov_token.conf");
    ck_assert_ptr_nonnull(conf);

    /* opencode_zen must read the shared "opencode" key. */
    const char *zen = conf_provider_token(conf, "opencode_zen");
    ck_assert_ptr_nonnull(zen);
    ck_assert_str_eq(zen, "sk-zen-1");

    /* Other providers keep their own key. */
    const char *openai = conf_provider_token(conf, "openai");
    ck_assert_ptr_nonnull(openai);
    ck_assert_str_eq(openai, "sk-openai-2");

    /* Provider without an entry resolves to NULL. */
    ck_assert_ptr_null(conf_provider_token(conf, "ollama"));

    conf_free(conf);
    ck_assert_int_eq(remove("/tmp/test_prov_token.conf"), 0);
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
    tcase_add_test(tc, test_conf_provider_tokens_all);
    tcase_add_test(tc, test_conf_provider_tokens_empty);
    tcase_add_test(tc, test_conf_provider_tokens_alloc_fail);
    tcase_add_test(tc, test_conf_provider_token_opencode_zen_aliases_opencode);
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
