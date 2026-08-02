#include <stdlib.h>
#include <check.h>

#include "../../src/llm/provider.h"

/* ================================================================
 *  get_provider tests
 * ================================================================ */

START_TEST(test_get_provider_ollama_returns_valid_provider)
{
    LLMProvider *p = get_provider("ollama", "model", "http://localhost:11434", NULL, 4096, 120);
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
    LLMProvider *p = get_provider("openai", "model", "https://api.openai.com", "sk-test-1", 0, 0);
    ck_assert_ptr_ne(p, NULL);
    ck_assert(p->chat != NULL);
    ck_assert(p->chat_streaming != NULL);
    ck_assert(p->extract_structured != NULL);
    ck_assert(p->destroy != NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_get_provider_openai_without_token_returns_valid_provider)
{
    /* Real OpenAI without a token is still constructible; the service
     * rejects the request with 401 at request time. */
    LLMProvider *p = get_provider("openai", "model", "https://api.openai.com", NULL, 0, 0);
    ck_assert_ptr_ne(p, NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_get_provider_openai_compatible_returns_valid_provider)
{
    LLMProvider *p = get_provider("openai_compatible", "model",
                                  "http://localhost:1234", "optional-token", 0, 0);
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
    /* Back-compat alias: lmstudio maps to the openai-compatible client. */
    LLMProvider *p = get_provider("lmstudio", "model", "http://localhost:1234", NULL, 0, 0);
    ck_assert_ptr_ne(p, NULL);
    ck_assert(p->chat != NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_get_provider_opencode_zen_returns_valid_provider)
{
    LLMProvider *p = get_provider("opencode_zen", "model",
                                  "https://opencode.ai/zen/v1", "zen-test-1", 0, 0);
    ck_assert_ptr_ne(p, NULL);
    ck_assert(p->chat != NULL);
    ck_assert(p->chat_streaming != NULL);
    ck_assert(p->extract_structured != NULL);
    ck_assert(p->destroy != NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_get_provider_opencode_zen_without_token_returns_valid_provider)
{
    /* Zen without a token is still constructible; the service rejects the
     * request with 401 at request time. */
    LLMProvider *p = get_provider("opencode_zen", "model", NULL, NULL, 0, 0);
    ck_assert_ptr_ne(p, NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_get_provider_unknown_returns_null)
{
    LLMProvider *p = get_provider("unknown_provider", "model", "url", NULL, 0, 0);
    ck_assert_ptr_eq(p, NULL);
}
END_TEST

START_TEST(test_get_provider_empty_name_returns_null)
{
    LLMProvider *p = get_provider("", "model", "url", NULL, 0, 0);
    ck_assert_ptr_eq(p, NULL);
}
END_TEST

START_TEST(test_get_provider_anthropic_not_implemented)
{
    LLMProvider *p = get_provider("anthropic", "model", "url", NULL, 0, 0);
    ck_assert_ptr_eq(p, NULL);
}
END_TEST

/* ================================================================
 *  provider_default_base_url tests
 * ================================================================ */

START_TEST(test_provider_default_base_url_known_providers)
{
    ck_assert_str_eq(provider_default_base_url("ollama"), "http://localhost:11434");
    ck_assert_str_eq(provider_default_base_url("openai"), "https://api.openai.com");
    ck_assert_str_eq(provider_default_base_url("openai_compatible"), "http://localhost:1234");
    ck_assert_str_eq(provider_default_base_url("opencode_zen"), "https://opencode.ai/zen/v1");
}
END_TEST

START_TEST(test_provider_default_base_url_unknown_returns_null)
{
    ck_assert_ptr_eq(provider_default_base_url("anthropic"), NULL);
    ck_assert_ptr_eq(provider_default_base_url(""), NULL);
    ck_assert_ptr_eq(provider_default_base_url(NULL), NULL);
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
    tcase_add_test(tc, test_get_provider_openai_without_token_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_openai_compatible_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_lmstudio_alias_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_opencode_zen_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_opencode_zen_without_token_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_unknown_returns_null);
    tcase_add_test(tc, test_get_provider_empty_name_returns_null);
    tcase_add_test(tc, test_get_provider_anthropic_not_implemented);
    suite_add_tcase(s, tc);

    TCase *tc_url = tcase_create("DefaultBaseUrl");
    tcase_add_test(tc_url, test_provider_default_base_url_known_providers);
    tcase_add_test(tc_url, test_provider_default_base_url_unknown_returns_null);
    suite_add_tcase(s, tc_url);

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
