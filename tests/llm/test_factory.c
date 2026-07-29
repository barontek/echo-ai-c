#include <stdlib.h>
#include <check.h>

#include "../../src/llm/provider.h"

/* Stub: lmstudio_provider_create is not linked; factory test only exercises the
 * ollama path and NULL-for-unknown path. */
LLMProvider *lmstudio_provider_create(const char *base_url)
{
    (void)base_url;
    return NULL;
}

/* ================================================================
 *  get_provider tests
 * ================================================================ */

START_TEST(test_get_provider_ollama_returns_valid_provider)
{
    LLMProvider *p = get_provider("ollama", "model", "http://localhost:11434", 4096, 120);
    ck_assert_ptr_ne(p, NULL);
    ck_assert_ptr_ne(p->chat, NULL);
    ck_assert_ptr_ne(p->chat_streaming, NULL);
    ck_assert_ptr_ne(p->extract_structured, NULL);
    ck_assert_ptr_ne(p->destroy, NULL);
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

/* ================================================================
 *  Suite
 * ================================================================ */

Suite *factory_suite(void)
{
    Suite *s = suite_create("factory");

    TCase *tc = tcase_create("GetProvider");
    tcase_add_test(tc, test_get_provider_ollama_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_unknown_returns_null);
    tcase_add_test(tc, test_get_provider_empty_name_returns_null);
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
