/* test_provider_models.c - parse and allocation-failure tests for the
 * shared model-list fetcher (provider_models.c). The curl transport is
 * exercised through the routes tests (test_routes_models.c); here the
 * pure JSON parse and its OOM path are under test. Depends on: check,
 * provider_models (PROVIDER_MODELS_TEST build).
 */

#define _GNU_SOURCE
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <check.h>
#include <stdlib.h>
#include <string.h>

#include "llm/provider_models.h"
#include "llm/openai.h"
#include "llm/openai_oauth.h"

/* ---- stubs for the openai transport path (not exercised here, but the
 * fetch function's references must link) ---- */

int openai_models_fetch_alloc(OpenAIOAuth *auth, char ***models_out,
                              size_t *count_out)
{
    (void)auth;
    if (models_out) *models_out = NULL;
    if (count_out) *count_out = 0U;
    return OPENAI_MODELS_UNAVAILABLE;
}

void openai_models_free(char **models, size_t count)
{
    if (!models) return;
    for (size_t i = 0; i < count; i++) free(models[i]);
    free(models);
}

OpenAIOAuthState openai_oauth_status(OpenAIOAuth *auth, char **account_id,
                                     char **plan_type, char **error)
{
    (void)auth;
    if (account_id) *account_id = NULL;
    if (plan_type) *plan_type = NULL;
    if (error) *error = NULL;
    return OPENAI_OAUTH_SIGNED_OUT;
}

const char *provider_default_base_url(const char *name)
{
    (void)name;
    return "http://localhost:11434";
}

/* ---- parse tests ---- */

START_TEST(test_parse_ollama_tags)
{
    const char *raw =
        "{\"models\":[{\"name\":\"llama3:latest\"},{\"name\":\"mistral:7b\"}]}";
    char **models = NULL;
    size_t count = 0U;
    ck_assert_int_eq(provider_models_parse_test(raw, "models", "name",
                                                &models, &count), 0);
    ck_assert_uint_eq(count, 2U);
    ck_assert_str_eq(models[0], "llama3:latest");
    ck_assert_str_eq(models[1], "mistral:7b");
    provider_models_free(models, count);
}

END_TEST

START_TEST(test_parse_openai_compatible_shape)
{
    /* OpenAI-compatible endpoints list under "data" with "id" keys. */
    const char *raw =
        "{\"data\":[{\"id\":\"qwen2.5\"},{\"id\":\"deepseek-v3\",\"owned_by\":\"x\"}]}";
    char **models = NULL;
    size_t count = 0U;
    ck_assert_int_eq(provider_models_parse_test(raw, "data", "id",
                                                &models, &count), 0);
    ck_assert_uint_eq(count, 2U);
    ck_assert_str_eq(models[0], "qwen2.5");
    ck_assert_str_eq(models[1], "deepseek-v3");
    provider_models_free(models, count);
}

END_TEST

START_TEST(test_parse_empty_array_success)
{
    char **models = (char **)(size_t)1; /* sentinel: must be reset to NULL */
    size_t count = 99U;
    ck_assert_int_eq(provider_models_parse_test("{\"models\":[]}", "models",
                                                "name", &models, &count), 0);
    ck_assert_ptr_null(models);
    ck_assert_uint_eq(count, 0U);
}

END_TEST

START_TEST(test_parse_malformed_is_empty_success)
{
    char **models = NULL;
    size_t count = 0U;
    ck_assert_int_eq(provider_models_parse_test("not json", "models",
                                                "name", &models, &count), 0);
    ck_assert_ptr_null(models);
    ck_assert_uint_eq(count, 0U);
}

END_TEST

START_TEST(test_parse_missing_list_key_is_empty_success)
{
    char **models = NULL;
    size_t count = 0U;
    ck_assert_int_eq(provider_models_parse_test("{\"other\":[1,2]}", "models",
                                                "name", &models, &count), 0);
    ck_assert_ptr_null(models);
    ck_assert_uint_eq(count, 0U);
}

END_TEST

START_TEST(test_parse_skips_non_string_entries)
{
    const char *raw =
        "{\"data\":[{\"id\":\"a\"},{\"id\":42},{\"foo\":\"b\"}]}";
    char **models = NULL;
    size_t count = 0U;
    ck_assert_int_eq(provider_models_parse_test(raw, "data", "id",
                                                &models, &count), 0);
    ck_assert_uint_eq(count, 1U);
    ck_assert_str_eq(models[0], "a");
    provider_models_free(models, count);
}

END_TEST

START_TEST(test_parse_null_arguments_rejected)
{
    char **models = NULL;
    size_t count = 0U;
    ck_assert_int_eq(provider_models_parse_test(NULL, "models", "name",
                                                &models, &count), -1);
    ck_assert_int_eq(provider_models_parse_test("{}", NULL, "name",
                                                &models, &count), -1);
    ck_assert_int_eq(provider_models_parse_test("{}", "models", NULL,
                                                &models, &count), -1);
}

END_TEST

START_TEST(test_parse_mid_list_oom_frees_partials)
{
    /* Regression for the multi-allocation commit site: a str_dup failure
     * mid-array must leave *models_out NULL and free every entry already
     * duplicated (ASan would flag a leak/use-after-free otherwise). */
    const char *raw =
        "{\"models\":[{\"name\":\"a\"},{\"name\":\"b\"},{\"name\":\"c\"}]}";
    provider_models_test_set_strdup_fail(2);
    char **models = (char **)(size_t)1;
    size_t count = 99U;
    ck_assert_int_eq(provider_models_parse_test(raw, "models", "name",
                                                &models, &count), -1);
    ck_assert_ptr_null(models);
    ck_assert_uint_eq(count, 0U);
    ck_assert_int_eq(provider_models_test_strdup_calls(), 2);
    /* All entries again: failure injection is off, parse succeeds. */
    provider_models_test_set_strdup_fail(-1);
    ck_assert_int_eq(provider_models_parse_test(raw, "models", "name",
                                                &models, &count), 0);
    ck_assert_uint_eq(count, 3U);
    provider_models_free(models, count);
}

END_TEST

Suite *provider_models_suite(void)
{
    Suite *s = suite_create("provider_models");
    TCase *tc = tcase_create("parse");
    tcase_add_test(tc, test_parse_ollama_tags);
    tcase_add_test(tc, test_parse_openai_compatible_shape);
    tcase_add_test(tc, test_parse_empty_array_success);
    tcase_add_test(tc, test_parse_malformed_is_empty_success);
    tcase_add_test(tc, test_parse_missing_list_key_is_empty_success);
    tcase_add_test(tc, test_parse_skips_non_string_entries);
    tcase_add_test(tc, test_parse_null_arguments_rejected);
    tcase_add_test(tc, test_parse_mid_list_oom_frees_partials);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = provider_models_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_ENV);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed;
}
