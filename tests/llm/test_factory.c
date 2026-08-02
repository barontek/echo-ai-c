#include <stdlib.h>
#include <check.h>

#include "../../src/llm/provider.h"

/* ================================================================
 *  get_provider tests
 * ================================================================ */

START_TEST(test_get_provider_ollama_returns_valid_provider)
{
    LLMProvider *p = get_provider("ollama", "model", "http://localhost:11434", 4096, 120);
    ck_assert_ptr_ne(p, NULL);
    ck_assert(p->chat != NULL);
    ck_assert(p->chat_streaming != NULL);
    ck_assert(p->extract_structured != NULL);
    ck_assert(p->destroy != NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_get_provider_openai_returns_valid_provider)
{
    LLMProvider *p = get_provider("openai", "model", "https://api.openai.com", 0, 0);
    ck_assert_ptr_ne(p, NULL);
    ck_assert(p->chat != NULL);
    ck_assert(p->chat_streaming != NULL);
    ck_assert(p->extract_structured != NULL);
    ck_assert(p->destroy != NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_get_provider_lmstudio_alias_returns_valid_provider)
{
    /* Back-compat alias: lmstudio maps to the openai client. */
    LLMProvider *p = get_provider("lmstudio", "model", "http://localhost:1234", 0, 0);
    ck_assert_ptr_ne(p, NULL);
    ck_assert(p->chat != NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_get_provider_unknown_returns_null)
{
    LLMProvider *p = get_provider("unknown_provider", "model", "url", 0, 0);
    ck_assert_ptr_eq(p, NULL);
}
END_TEST

START_TEST(test_get_provider_empty_name_returns_null)
{
    LLMProvider *p = get_provider("", "model", "url", 0, 0);
    ck_assert_ptr_eq(p, NULL);
}
END_TEST

START_TEST(test_get_provider_anthropic_not_implemented)
{
    LLMProvider *p = get_provider("anthropic", "model", "url", 0, 0);
    ck_assert_ptr_eq(p, NULL);
}
END_TEST

/* ================================================================
 *  Suite
 * ================================================================ */

Suite *factory_suite(void)
{
    Suite *s = suite_create("factory");

    TCase *tc = tcase_create("GetProvider");
    tcase_add_test(tc, test_get_provider_ollama_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_openai_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_lmstudio_alias_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_unknown_returns_null);
    tcase_add_test(tc, test_get_provider_empty_name_returns_null);
    tcase_add_test(tc, test_get_provider_anthropic_not_implemented);
    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s = factory_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}
