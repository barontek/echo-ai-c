/* test_routes_general_models.c - routes general handle_models tests
 * Split from test_routes_general.c (2026-08 file-length compliance);
 * shared stubs and fixtures live in test_routes_general_fixture.c.
 * Depends on: check, routes_general.
 */

#define _GNU_SOURCE
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <check.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <cjson/cJSON.h>

#include "test_routes_general_fixture.h"

START_TEST(test_handle_models_curl_unavailable)
{
    stub_curl_init_nonnull = 0;
    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"models\":[]"));

    reset_stubs();
}

START_TEST(test_handle_models_perform_fails)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_URL_MALFORMAT;
    stub_models_json = NULL;
    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"models\":[]"));

    reset_stubs();
}

START_TEST(test_handle_models_success)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"models\":[{\"name\":\"llama3:latest\"},{\"name\":\"mistral:7b\"}]}";
    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "llama3:latest"));
    ck_assert(strstr(captured_body, "mistral:7b"));

    reset_stubs();
}

START_TEST(test_handle_models_empty_models_array)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"models\":[]}";
    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"models\":[]"));

    reset_stubs();
}

START_TEST(test_handle_models_ollama_default_url)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"models\":[{\"name\":\"llama3:latest\"}]}";
    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_curl_url, "http://localhost:11434/api/tags"));
    ck_assert(strstr(captured_body, "llama3:latest"));

    reset_stubs();
}

START_TEST(test_handle_models_ollama_explicit_query)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"models\":[{\"name\":\"llama3:latest\"}]}";
    ServerContext ctx = {0};
    HTTPRequest req = {0};
    strcpy(req.query, "provider=ollama&foo=1");

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_curl_url, "http://localhost:11434/api/tags"));

    reset_stubs();
}

START_TEST(test_handle_models_openai_signed_out_returns_empty_local_list)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"data\":[{\"id\":\"qwen2.5\"},{\"id\":\"deepseek-v3\"}]}";
    ServerContext ctx = {0};
    HTTPRequest req = {0};
    strcpy(req.query, "provider=openai");

    ctx.openai_oauth = (OpenAIOAuth *)1;
    openai_oauth_stub_state = OPENAI_OAUTH_SIGNED_OUT;
    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert_str_eq(captured_curl_url, "");
    ck_assert_str_eq(captured_body, "{\"models\":[]}");

    reset_stubs();
}

START_TEST(test_handle_models_openai_signed_in_uses_remote_catalog)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"data\":[{\"id\":\"gpt-4o-mini\"}]}";

    char tmpdir[] = "/tmp/test_models_token_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char conf_path[512];
    snprintf(conf_path, sizeof(conf_path), "%s/test.conf", tmpdir);
    FILE *f = fopen(conf_path, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "[providers]\nopenai = sk-model-test\n");
    fclose(f);

    Conf *conf = conf_load(conf_path);
    ck_assert_ptr_nonnull(conf);
    ServerContext ctx = {0};
    ctx.conf = conf;
    ctx.openai_oauth = (OpenAIOAuth *)1;
    openai_oauth_stub_state = OPENAI_OAUTH_SIGNED_IN;
    stub_openai_models_result = 0;
    stub_openai_models[0] = "gpt-5.4";
    stub_openai_models[1] = "gpt-5.3-codex";
    stub_openai_models[2] = "gpt-5.2-codex";
    stub_openai_models_count = 3U;
    HTTPRequest req = {0};
    strcpy(req.query, "provider=openai");

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert_str_eq(captured_auth_header, "");
    ck_assert_str_eq(captured_curl_url, "");
    ck_assert(strstr(captured_body, "gpt-5.4"));
    ck_assert(strstr(captured_body, "gpt-5.3-codex"));
    ck_assert(strstr(captured_body, "gpt-5.2-codex"));

    conf_free(conf);
    char rm[512];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int syst = system(rm);
    (void)syst;
    reset_stubs();
}

START_TEST(test_handle_models_openai_discovery_failure_uses_fallback)
{
    ServerContext ctx = {0};
    ctx.openai_oauth = (OpenAIOAuth *)1;
    openai_oauth_stub_state = OPENAI_OAUTH_SIGNED_IN;
    stub_openai_models_result = -1;
    HTTPRequest req = {0};
    strcpy(req.query, "provider=openai");

    handle_models(&req, NULL, &ctx);

    ck_assert_int_eq(captured_status, 200);
    ck_assert_ptr_nonnull(strstr(captured_body, "gpt-5.5"));
    ck_assert_ptr_nonnull(strstr(captured_body, "gpt-5.3-codex-spark"));
    reset_stubs();
}

START_TEST(test_handle_models_openai_denied_returns_empty_list)
{
    /* 4xx entitlement denial must not surface the fallback catalog. */
    ServerContext ctx = {0};
    ctx.openai_oauth = (OpenAIOAuth *)1;
    openai_oauth_stub_state = OPENAI_OAUTH_SIGNED_IN;
    stub_openai_models_result = OPENAI_MODELS_DENIED;
    HTTPRequest req = {0};
    strcpy(req.query, "provider=openai");

    handle_models(&req, NULL, &ctx);

    ck_assert_int_eq(captured_status, 200);
    ck_assert_str_eq(captured_body, "{\"models\":[]}");
    ck_assert_ptr_null(strstr(captured_body, "gpt-5.5"));
    reset_stubs();
}

START_TEST(test_handle_models_openai_zero_visible_returns_empty_list)
{
    /* A parsed-but-empty catalog must not fall back to the fixed list. */
    ServerContext ctx = {0};
    ctx.openai_oauth = (OpenAIOAuth *)1;
    openai_oauth_stub_state = OPENAI_OAUTH_SIGNED_IN;
    stub_openai_models_result = OPENAI_MODELS_OK;
    stub_openai_models_count = 0U;
    HTTPRequest req = {0};
    strcpy(req.query, "provider=openai");

    handle_models(&req, NULL, &ctx);

    ck_assert_int_eq(captured_status, 200);
    ck_assert_str_eq(captured_body, "{\"models\":[]}");
    ck_assert_ptr_null(strstr(captured_body, "gpt-5.5"));
    reset_stubs();
}

START_TEST(test_handle_models_lm_studio_alias_uses_openai)
{
    /* lm_studio is an alias for the static-token compatible endpoint. */
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"data\":[{\"id\":\"qwen2.5\"}]}";
    ServerContext ctx = {0};
    HTTPRequest req = {0};
    strcpy(req.query, "provider=lm_studio");

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_curl_url, "http://localhost:1234/v1/models"));
    ck_assert(strstr(captured_body, "qwen2.5"));

    reset_stubs();
}

START_TEST(test_handle_models_openai_ignores_custom_public_base_url)
{
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"data\":[{\"id\":\"local-model\"}]}";

    char tmpdir[] = "/tmp/test_models_conf_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char conf_path[512];
    snprintf(conf_path, sizeof(conf_path), "%s/test.conf", tmpdir);
    FILE *f = fopen(conf_path, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "[openai]\nbase_url = http://localhost:1234\n");
    fclose(f);

    Conf *conf = conf_load(conf_path);
    ck_assert_ptr_nonnull(conf);
    ServerContext ctx = {0};
    ctx.conf = conf;
    ctx.openai_oauth = (OpenAIOAuth *)1;
    openai_oauth_stub_state = OPENAI_OAUTH_SIGNED_IN;
    stub_openai_models_result = 0;
    stub_openai_models[0] = "gpt-5.4";
    stub_openai_models_count = 1U;
    HTTPRequest req = {0};
    strcpy(req.query, "provider=openai");

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert_str_eq(captured_curl_url, "");
    ck_assert(strstr(captured_body, "gpt-5.4"));

    conf_free(conf);
    char rm[512];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int syst = system(rm);
    (void)syst;
    reset_stubs();
}

START_TEST(test_handle_models_openai_compatible_uses_own_base_url)
{
    /* openai_compatible resolves its own base_url key, not openai's. */
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"data\":[{\"id\":\"local-model\"}]}";

    char tmpdir[] = "/tmp/test_models_conf_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char conf_path[512];
    snprintf(conf_path, sizeof(conf_path), "%s/test.conf", tmpdir);
    FILE *f = fopen(conf_path, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "[openai_compatible]\nbase_url = http://localhost:4321\n");
    fclose(f);

    Conf *conf = conf_load(conf_path);
    ck_assert_ptr_nonnull(conf);
    ServerContext ctx = {0};
    ctx.conf = conf;
    HTTPRequest req = {0};
    strcpy(req.query, "provider=openai_compatible");

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_curl_url, "http://localhost:4321/v1/models"));
    ck_assert(strstr(captured_body, "local-model"));

    conf_free(conf);
    char rm[512];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int syst = system(rm);
    (void)syst;
    reset_stubs();
}

START_TEST(test_handle_models_opencode_go_uses_own_base_url)
{
    /* Regression: opencode_go used to hit the "unsupported provider" branch
     * and return an empty list, so the UI showed "no models available". */
    stub_curl_init_nonnull = 1;
    stub_curl_perform_code = CURLE_OK;
    stub_models_json = "{\"data\":[{\"id\":\"minimax-m3\"}]}";

    char tmpdir[] = "/tmp/test_models_conf_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char conf_path[512];
    snprintf(conf_path, sizeof(conf_path), "%s/test.conf", tmpdir);
    FILE *f = fopen(conf_path, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "[opencode_go]\nbase_url = https://opencode.ai/zen/go/v1\n");
    fclose(f);

    Conf *conf = conf_load(conf_path);
    ck_assert_ptr_nonnull(conf);
    ServerContext ctx = {0};
    ctx.conf = conf;
    HTTPRequest req = {0};
    strcpy(req.query, "provider=opencode_go");

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_curl_url, "https://opencode.ai/zen/go/v1/models"));
    ck_assert(strstr(captured_body, "minimax-m3"));

    conf_free(conf);
    char rm[512];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int syst = system(rm);
    (void)syst;
    reset_stubs();
}

START_TEST(test_handle_models_unknown_provider_empty_no_curl)
{
    stub_curl_init_nonnull = 1;
    ServerContext ctx = {0};
    HTTPRequest req = {0};
    strcpy(req.query, "provider=anthropic");

    handle_models(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"models\":[]"));
    ck_assert_str_eq(captured_curl_url, "");

    reset_stubs();
}


Suite *routes_general_models_suite(void)
{
    Suite *s = suite_create("routes_general_models");

    TCase *tc = tcase_create("handle_models");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_models_curl_unavailable);
    tcase_add_test(tc, test_handle_models_perform_fails);
    tcase_add_test(tc, test_handle_models_success);
    tcase_add_test(tc, test_handle_models_empty_models_array);
    tcase_add_test(tc, test_handle_models_ollama_default_url);
    tcase_add_test(tc, test_handle_models_ollama_explicit_query);
    tcase_add_test(tc, test_handle_models_openai_signed_out_returns_empty_local_list);
    tcase_add_test(tc, test_handle_models_openai_signed_in_uses_remote_catalog);
    tcase_add_test(tc, test_handle_models_openai_discovery_failure_uses_fallback);
    tcase_add_test(tc, test_handle_models_openai_denied_returns_empty_list);
    tcase_add_test(tc, test_handle_models_openai_zero_visible_returns_empty_list);
    tcase_add_test(tc, test_handle_models_lm_studio_alias_uses_openai);
    tcase_add_test(tc, test_handle_models_openai_ignores_custom_public_base_url);
    tcase_add_test(tc, test_handle_models_openai_compatible_uses_own_base_url);
    tcase_add_test(tc, test_handle_models_opencode_go_uses_own_base_url);
    tcase_add_test(tc, test_handle_models_unknown_provider_empty_no_curl);

    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    Suite *s = routes_general_models_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_ENV);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed;
}
