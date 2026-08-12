#include <stdlib.h>
#include <check.h>

#include "../../src/llm/factory.h"
#include "../../src/llm/provider.h"
#include "../../src/llm/openai_oauth.h"

/* test_factory - unit tests for factory. Depends on: check, the module under test. */
/* ================================================================
 *  get_provider tests
 * ================================================================ */

START_TEST(test_get_provider_ollama_returns_valid_provider)
{
    LLMProvider *p = get_provider("ollama", "model", "http://localhost:11434", NULL, 4096, 120, NULL);
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
    OpenAIOAuth *auth = (OpenAIOAuth *)1;
    LLMProvider *p = get_provider_with_auth("openai", "model", NULL,
                                            "ignored", 0, 0, NULL, auth);
    ck_assert_ptr_ne(p, NULL);
    ck_assert(p->chat != NULL);
    ck_assert(p->chat_streaming != NULL);
    ck_assert(p->extract_structured != NULL);
    ck_assert(p->destroy != NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_get_provider_openai_with_effort_returns_valid_provider)
{
    OpenAIOAuth *auth = (OpenAIOAuth *)1;
    LLMProvider *p = get_provider_with_auth("openai", "model", NULL,
                                            "ignored", 0, 0, "high", auth);
    ck_assert_ptr_ne(p, NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_get_provider_openai_with_invalid_effort_returns_null)
{
    OpenAIOAuth *auth = (OpenAIOAuth *)1;
    LLMProvider *p = get_provider_with_auth("openai", "model", NULL,
                                            "ignored", 0, 0, "extreme", auth);
    ck_assert_ptr_null(p);
}
END_TEST

START_TEST(test_get_provider_openai_without_oauth_returns_null)
{
    LLMProvider *p = get_provider("openai", "model", "https://api.openai.com", NULL, 0, 0, NULL);
    ck_assert_ptr_null(p);
}
END_TEST

START_TEST(test_get_provider_openai_compatible_returns_valid_provider)
{
    LLMProvider *p = get_provider("openai_compatible", "model",
                                  "http://localhost:1234", "optional-token", 0, 0, NULL);
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
    LLMProvider *p = get_provider("lmstudio", "model", "http://localhost:1234", NULL, 0, 0, NULL);
    ck_assert_ptr_ne(p, NULL);
    ck_assert(p->chat != NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_get_provider_opencode_zen_returns_valid_provider)
{
    LLMProvider *p = get_provider("opencode_zen", "model",
                                  "https://opencode.ai/zen/v1", "zen-test-1", 0, 0, NULL);
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
    LLMProvider *p = get_provider("opencode_zen", "model", NULL, NULL, 0, 0, NULL);
    ck_assert_ptr_ne(p, NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_get_provider_opencode_zen_accepts_effort)
{
    LLMProvider *p = get_provider("opencode_zen", "model", NULL, "zen-test-1",
                                  0, 0, "none");
    ck_assert_ptr_ne(p, NULL);
    p->destroy(p);
    /* openai-only value stays rejected for zen. */
    p = get_provider("opencode_zen", "model", NULL, "zen-test-1", 0, 0, "xhigh");
    ck_assert_ptr_null(p);
}
END_TEST

START_TEST(test_get_provider_opencode_go_returns_valid_provider)
{
    /* Go is the OpenAI-compatible client at the go gateway: constructible
     * with or without a token, and the base_url defaults to the go
     * endpoint rather than openai_compatible's localhost. */
    LLMProvider *p = get_provider("opencode_go", "model", NULL, "go-test-1", 0, 0, NULL);
    ck_assert_ptr_ne(p, NULL);
    ck_assert(p->chat != NULL);
    ck_assert(p->chat_streaming != NULL);
    ck_assert(p->destroy != NULL);
    p->destroy(p);

    p = get_provider("opencode_go", "model", NULL, NULL, 0, 0, NULL);
    ck_assert_ptr_ne(p, NULL);
    p->destroy(p);
}
END_TEST

START_TEST(test_get_provider_opencode_go_accepts_effort)
{
    LLMProvider *p = get_provider("opencode_go", "model", NULL, "go-test-1",
                                  0, 0, "none");
    ck_assert_ptr_ne(p, NULL);
    p->destroy(p);
    /* openai-only value stays rejected for go. */
    p = get_provider("opencode_go", "model", NULL, "go-test-1", 0, 0, "xhigh");
    ck_assert_ptr_null(p);
}
END_TEST

START_TEST(test_get_provider_unknown_returns_null)
{
    LLMProvider *p = get_provider("unknown_provider", "model", "url", NULL, 0, 0, NULL);
    ck_assert_ptr_eq(p, NULL);
}
END_TEST

START_TEST(test_get_provider_empty_name_returns_null)
{
    LLMProvider *p = get_provider("", "model", "url", NULL, 0, 0, NULL);
    ck_assert_ptr_eq(p, NULL);
}
END_TEST

START_TEST(test_get_provider_anthropic_not_implemented)
{
    LLMProvider *p = get_provider("anthropic", "model", "url", NULL, 0, 0, NULL);
    ck_assert_ptr_eq(p, NULL);
}
END_TEST

/* ================================================================
 *  provider_default_base_url tests
 * ================================================================ */

START_TEST(test_provider_default_base_url_known_providers)
{
    ck_assert_str_eq(provider_default_base_url("ollama"), "http://localhost:11434");
    ck_assert_str_eq(provider_default_base_url("openai"),
                     "https://chatgpt.com/backend-api/codex/responses");
    ck_assert_str_eq(provider_default_base_url("openai_compatible"), "http://localhost:1234");
    ck_assert_str_eq(provider_default_base_url("opencode_zen"), "https://opencode.ai/zen/v1");
    ck_assert_str_eq(provider_default_base_url("opencode_go"), "https://opencode.ai/zen/go/v1");
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
 *  provider_supports_effort tests
 * ================================================================ */

START_TEST(test_provider_supports_effort_lists)
{
    ck_assert_int_eq(provider_supports_effort("openai"), 1);
    ck_assert_int_eq(provider_supports_effort("openai_compatible"), 1);
    ck_assert_int_eq(provider_supports_effort("ollama"), 1);
    ck_assert_int_eq(provider_supports_effort("opencode_zen"), 1);
    ck_assert_int_eq(provider_supports_effort("opencode_go"), 1);
}
END_TEST

START_TEST(test_provider_supports_effort_unknown_returns_zero)
{
    ck_assert_int_eq(provider_supports_effort("anthropic"), 0);
    ck_assert_int_eq(provider_supports_effort(""), 0);
    ck_assert_int_eq(provider_supports_effort(NULL), 0);
}
END_TEST

START_TEST(test_provider_effort_options_are_provider_specific)
{
    const char *const *openai = provider_effort_options("openai");
    ck_assert_ptr_nonnull(openai);
    ck_assert_str_eq(openai[0], "low");
    ck_assert_str_eq(openai[3], "xhigh");  /* openai-only value */
    ck_assert_str_eq(openai[5], "none");
    ck_assert_ptr_null(openai[6]);

    const char *const *compat = provider_effort_options("openai_compatible");
    ck_assert_ptr_nonnull(compat);
    ck_assert_str_eq(compat[0], "low");
    ck_assert_str_eq(compat[3], "max");
    ck_assert_str_eq(compat[4], "none");
    ck_assert_ptr_null(compat[5]);

    const char *const *ollama = provider_effort_options("ollama");
    ck_assert_ptr_nonnull(ollama);
    ck_assert_str_eq(ollama[0], "low");
    ck_assert_str_eq(ollama[4], "none");
    ck_assert_ptr_null(ollama[5]);

    /* Zen is the OpenAI-compatible client: same list, no xhigh. */
    const char *const *zen = provider_effort_options("opencode_zen");
    ck_assert_ptr_nonnull(zen);
    ck_assert_str_eq(zen[0], "low");
    ck_assert_str_eq(zen[3], "max");
    ck_assert_str_eq(zen[4], "none");
    ck_assert_ptr_null(zen[5]);
}

START_TEST(test_provider_effort_valid_checks_the_provider_set)
{
    /* NULL/empty always valid. */
    ck_assert_int_eq(provider_effort_valid("openai", NULL), 1);
    ck_assert_int_eq(provider_effort_valid("openai", ""), 1);
    ck_assert_int_eq(provider_effort_valid("ollama", NULL), 1);
    /* openai-only value. */
    ck_assert_int_eq(provider_effort_valid("openai", "xhigh"), 1);
    ck_assert_int_eq(provider_effort_valid("openai_compatible", "xhigh"), 0);
    ck_assert_int_eq(provider_effort_valid("ollama", "xhigh"), 0);
    /* shared values. */
    ck_assert_int_eq(provider_effort_valid("ollama", "none"), 1);
    ck_assert_int_eq(provider_effort_valid("openai_compatible", "max"), 1);
    ck_assert_int_eq(provider_effort_valid("opencode_zen", "max"), 1);
    ck_assert_int_eq(provider_effort_valid("opencode_zen", "xhigh"), 0);
    /* dropped value. */
    ck_assert_int_eq(provider_effort_valid("openai", "minimal"), 0);
    /* unknown provider. */
    ck_assert_int_eq(provider_effort_valid("anthropic", "low"), 0);
    ck_assert_int_eq(provider_effort_valid(NULL, "low"), 0);
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
    tcase_add_test(tc, test_get_provider_openai_with_effort_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_openai_with_invalid_effort_returns_null);
    tcase_add_test(tc, test_get_provider_openai_without_oauth_returns_null);
    tcase_add_test(tc, test_get_provider_openai_compatible_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_lmstudio_alias_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_opencode_zen_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_opencode_zen_without_token_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_opencode_zen_accepts_effort);
    tcase_add_test(tc, test_get_provider_opencode_go_returns_valid_provider);
    tcase_add_test(tc, test_get_provider_opencode_go_accepts_effort);
    tcase_add_test(tc, test_get_provider_unknown_returns_null);
    tcase_add_test(tc, test_get_provider_empty_name_returns_null);
    tcase_add_test(tc, test_get_provider_anthropic_not_implemented);
    suite_add_tcase(s, tc);

    TCase *tc_url = tcase_create("DefaultBaseUrl");
    tcase_add_test(tc_url, test_provider_default_base_url_known_providers);
    tcase_add_test(tc_url, test_provider_default_base_url_unknown_returns_null);
    tcase_add_test(tc_url, test_provider_supports_effort_lists);
    tcase_add_test(tc_url, test_provider_supports_effort_unknown_returns_zero);
    tcase_add_test(tc_url, test_provider_effort_options_are_provider_specific);
    tcase_add_test(tc_url, test_provider_effort_valid_checks_the_provider_set);
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
