#include <check.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/openai_oauth.h"
#include "utils/logging.h"

/* test_openai_oauth - unit tests for openai oauth. Depends on: check, the module under test. */
static const char *stored_oauth_payload = NULL;
static int delete_oauth_result = 0;

/* This test is intentionally self-contained so it can be compiled directly
 * while the shared beta workspace is not allowed to change CMake files. */
int session_manager_save_provider_oauth(SessionManager *session,
                                        const char *provider, const char *data)
{
    (void)session;
    (void)provider;
    (void)data;
    return 0;
}

char *session_manager_load_provider_oauth_alloc(SessionManager *session,
                                           const char *provider)
{
    (void)session;
    (void)provider;
    return NULL;
}

ProviderOAuthLoadResult session_manager_load_provider_oauth_ex(
    SessionManager *session, const char *provider, char **data_out)
{
    (void)session;
    (void)provider;
    if (!data_out) return PROVIDER_OAUTH_LOAD_INVALID_ARGUMENT;
    *data_out = NULL;
    if (!stored_oauth_payload) return PROVIDER_OAUTH_LOAD_NOT_FOUND;
    size_t length = strlen(stored_oauth_payload);
    *data_out = malloc(length + 1U);
    if (*data_out) memcpy(*data_out, stored_oauth_payload, length + 1U);
    return *data_out ? PROVIDER_OAUTH_LOAD_OK : PROVIDER_OAUTH_LOAD_OOM;
}

int session_manager_delete_provider_oauth(SessionManager *session,
                                          const char *provider)
{
    (void)session;
    (void)provider;
    return delete_oauth_result;
}

void session_manager_lock(SessionManager *session)
{
    (void)session;
}

void session_manager_unlock(SessionManager *session)
{
    (void)session;
}

void log_msg(LogLevel level, const char *file, int line, const char *message, ...)
{
    (void)level;
    (void)file;
    (void)line;
    (void)message;
}

static void free_fields(char **first, char **second, char **third)
{
    free(*first);
    free(*second);
    free(*third);
    *first = NULL;
    *second = NULL;
    *third = NULL;
}

static int parse_request(const void *request, size_t len, char **code,
                         char **state, char **denial)
{
    return openai_oauth_test_parse_callback(request, len, code, state, denial);
}

START_TEST(test_pkce_matches_rfc7636_s256_vector)
{
    const char *verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    char *challenge = openai_oauth_test_pkce_challenge_alloc(verifier);
    ck_assert_ptr_nonnull(challenge);
    ck_assert_str_eq(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    free(challenge);
}
END_TEST

START_TEST(test_logout_keeps_credentials_when_durable_delete_fails)
{
    stored_oauth_payload =
        "{\"access_token\":\"access\",\"refresh_token\":\"refresh\","
        "\"expires_at\":4102444800}";
    OpenAIOAuth *auth = openai_oauth_create();
    ck_assert_ptr_nonnull(auth);
    ck_assert_int_eq(openai_oauth_attach_session(auth, (SessionManager *)1), 0);
    ck_assert_int_eq(openai_oauth_status(auth, NULL, NULL, NULL),
                     OPENAI_OAUTH_SIGNED_IN);

    delete_oauth_result = -1;
    ck_assert_int_eq(openai_oauth_logout(auth), -1);
    ck_assert_int_eq(openai_oauth_status(auth, NULL, NULL, NULL),
                     OPENAI_OAUTH_SIGNED_IN);
    delete_oauth_result = 0;
    ck_assert_int_eq(openai_oauth_logout(auth), 0);
    ck_assert_int_eq(openai_oauth_status(auth, NULL, NULL, NULL),
                     OPENAI_OAUTH_SIGNED_OUT);

    stored_oauth_payload = NULL;
    openai_oauth_destroy(auth);
}
END_TEST

START_TEST(test_authorize_url_contains_encoded_required_parameters)
{
    char *url = openai_oauth_test_build_authorize_url_alloc("state value&one", "challenge/value");
    ck_assert_ptr_nonnull(url);
    ck_assert_ptr_nonnull(strstr(url, "https://auth.openai.com/oauth/authorize?"));
    ck_assert_ptr_nonnull(strstr(url, "response_type=code"));
    ck_assert_ptr_nonnull(strstr(url, "redirect_uri=http%3A%2F%2Flocalhost%3A1455%2Fauth%2Fcallback"));
    ck_assert_ptr_nonnull(strstr(url, "scope=openid%20profile%20email%20offline_access"));
    ck_assert_ptr_nonnull(strstr(url, "code_challenge=challenge%2Fvalue"));
    ck_assert_ptr_nonnull(strstr(url, "code_challenge_method=S256"));
    ck_assert_ptr_nonnull(strstr(url, "id_token_add_organizations=true"));
    ck_assert_ptr_nonnull(strstr(url, "codex_cli_simplified_flow=true"));
    ck_assert_ptr_nonnull(strstr(url, "originator=echo-ai"));
    ck_assert_ptr_nonnull(strstr(url, "state=state%20value%26one"));
    free(url);
}
END_TEST

START_TEST(test_callback_parser_accepts_exact_success_request)
{
    const char request[] =
        "GET /auth/callback?code=code%2D123&state=state%5F456 HTTP/1.1\r\n"
        "Host: localhost:1455\r\nUser-Agent: test\r\n\r\n";
    char *code = NULL;
    char *state = NULL;
    char *denial = NULL;
    ck_assert_int_eq(parse_request(request, sizeof(request) - 1,
                                   &code, &state, &denial), 0);
    ck_assert_str_eq(code, "code-123");
    ck_assert_str_eq(state, "state_456");
    ck_assert_ptr_null(denial);
    free_fields(&code, &state, &denial);
}
END_TEST

START_TEST(test_callback_parser_accepts_denial_as_error_state)
{
    const char request[] =
        "GET /auth/callback?error=access_denied&error_description=No%20thanks&state=s HTTP/1.1\r\n"
        "Host: localhost:1455\r\n\r\n";
    char *code = NULL;
    char *state = NULL;
    char *denial = NULL;
    ck_assert_int_eq(parse_request(request, sizeof(request) - 1,
                                   &code, &state, &denial), 0);
    ck_assert_ptr_null(code);
    ck_assert_str_eq(state, "s");
    ck_assert_str_eq(denial, "access_denied");
    free_fields(&code, &state, &denial);
}
END_TEST

START_TEST(test_callback_parser_rejects_duplicate_and_malformed_fields)
{
    const char *requests[] = {
        "GET /auth/callback?code=a&code=b&state=s HTTP/1.1\r\nHost: localhost:1455\r\n\r\n",
        "GET /auth/callback?code=a&state=s&state=t HTTP/1.1\r\nHost: localhost:1455\r\n\r\n",
        "GET /auth/callback?code=a&state=%ZZ HTTP/1.1\r\nHost: localhost:1455\r\n\r\n",
        "GET /auth/callback?code=a&state=%00bad HTTP/1.1\r\nHost: localhost:1455\r\n\r\n",
        "GET /auth/callback?code=a&&state=s HTTP/1.1\r\nHost: localhost:1455\r\n\r\n",
        "GET /auth/callback?code=a&state=s&error=no HTTP/1.1\r\nHost: localhost:1455\r\n\r\n",
        "GET /auth/callback?code=a HTTP/1.1\r\nHost: localhost:1455\r\n\r\n"
    };
    for (size_t index = 0; index < sizeof(requests) / sizeof(requests[0]); index++)
    {
        char *code = NULL;
        char *state = NULL;
        char *denial = NULL;
        ck_assert_int_eq(parse_request(requests[index], strlen(requests[index]),
                                       &code, &state, &denial), -1);
        ck_assert_ptr_null(code);
        ck_assert_ptr_null(state);
        ck_assert_ptr_null(denial);
    }
}
END_TEST

START_TEST(test_callback_parser_rejects_wrong_method_path_and_smuggling)
{
    const char *requests[] = {
        "POST /auth/callback?code=a&state=s HTTP/1.1\r\nHost: localhost:1455\r\n\r\n",
        "GET /auth/callback/extra?code=a&state=s HTTP/1.1\r\nHost: localhost:1455\r\n\r\n",
        "GET /auth/callback?code=a&state=s HTTP/1.0\r\nHost: localhost:1455\r\n\r\n",
        "GET /auth/callback?code=a&state=s HTTP/1.1\nHost: localhost:1455\n\n",
        "GET /auth/callback?code=a&state=s HTTP/1.1\r\nHost: localhost:1455\r\nContent-Length: 0\r\n\r\n",
        "GET /auth/callback?code=a&state=s HTTP/1.1\r\nHost: localhost:1455\r\nTransfer-Encoding: chunked\r\n\r\n",
        "GET /auth/callback?code=a&state=s HTTP/1.1\r\nUser-Agent: test\r\n\r\n",
        "GET /auth/callback?code=a&state=s HTTP/1.1\r\nHost: attacker.example\r\n\r\n",
        "GET /auth/callback?code=a&state=s HTTP/1.1\r\nHost: localhost:1455\r\nHost: localhost:1455\r\n\r\n",
        "GET /auth/callback?code=a&state=s HTTP/1.1\r\nHost: localhost:1455\r\n\r\nGET /evil HTTP/1.1\r\n\r\n"
    };
    for (size_t index = 0; index < sizeof(requests) / sizeof(requests[0]); index++)
    {
        char *code = NULL;
        char *state = NULL;
        char *denial = NULL;
        ck_assert_int_eq(parse_request(requests[index], strlen(requests[index]),
                                       &code, &state, &denial), -1);
    }
}
END_TEST

START_TEST(test_callback_parser_rejects_embedded_nul_and_oversize)
{
    char nul_request[] =
        "GET /auth/callback?code=a&state=s HTTP/1.1\r\nHost: x\r\n\r\n";
    nul_request[10] = '\0';
    char *code = NULL;
    char *state = NULL;
    char *denial = NULL;
    ck_assert_int_eq(parse_request(nul_request, sizeof(nul_request) - 1,
                                   &code, &state, &denial), -1);

    unsigned char *oversize = malloc(8193);
    ck_assert_ptr_nonnull(oversize);
    memset(oversize, 'A', 8193);
    ck_assert_int_eq(parse_request(oversize, 8193, &code, &state, &denial), -1);
    free(oversize);
}
END_TEST

START_TEST(test_token_parser_validates_and_extracts_jwt_metadata)
{
    const char *jwt =
        "eyJhbGciOiJub25lIn0."
        "eyJjaGF0Z3B0X2FjY291bnRfaWQiOiJhY2N0LTEiLCJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9wbGFuX3R5cGUiOiJwbHVzIn19."
        "sig";
    char json[1024] = {0};
    int written = snprintf(json, sizeof(json),
        "{\"access_token\":\"access\",\"refresh_token\":\"refresh\","
        "\"expires_in\":3600,\"id_token\":\"%s\"}", jwt);
    ck_assert_int_gt(written, 0);
    ck_assert_int_lt(written, (int)sizeof(json));
    char *access = NULL;
    char *refresh = NULL;
    char *account = NULL;
    char *plan = NULL;
    time_t expires_at = 0;
    ck_assert_int_eq(openai_oauth_test_parse_token(json, 0, (time_t)1000,
        &access, &refresh, &account, &plan, &expires_at), 0);
    ck_assert_str_eq(access, "access");
    ck_assert_str_eq(refresh, "refresh");
    ck_assert_str_eq(account, "acct-1");
    ck_assert_str_eq(plan, "plus");
    ck_assert_int_eq(expires_at, (time_t)4600);
    free(access);
    free(refresh);
    free(account);
    free(plan);
}
END_TEST

START_TEST(test_token_parser_reuses_refresh_token_only_for_refresh_response)
{
    const char json[] = "{\"access_token\":\"next\",\"expires_in\":600}";
    char *access = NULL;
    char *refresh = NULL;
    char *account = NULL;
    char *plan = NULL;
    time_t expires_at = 0;
    ck_assert_int_eq(openai_oauth_test_parse_token(json, 0, 100,
        &access, &refresh, &account, &plan, &expires_at), -1);
    ck_assert_int_eq(openai_oauth_test_parse_token(json, 1, 100,
        &access, &refresh, &account, &plan, &expires_at), 0);
    ck_assert_str_eq(access, "next");
    ck_assert_str_eq(refresh, "old-refresh");
    ck_assert_int_eq(expires_at, (time_t)700);
    free(access);
    free(refresh);
}
END_TEST

START_TEST(test_token_parser_rejects_missing_invalid_and_trailing_data)
{
    const char *invalid[] = {
        "{\"refresh_token\":\"r\",\"expires_in\":60}",
        "{\"access_token\":\"a\",\"refresh_token\":\"r\"}",
        "{\"access_token\":\"a\",\"refresh_token\":\"r\",\"expires_in\":0}",
        "{\"access_token\":\"a\",\"refresh_token\":\"r\",\"expires_in\":1.5}",
        "{\"access_token\":\"a\",\"refresh_token\":\"r\",\"expires_in\":60,\"id_token\":\"bad\"}",
        "{\"access_token\":\"a\",\"access_token\":\"b\",\"refresh_token\":\"r\",\"expires_in\":60}",
        "{\"access_token\":\"a\",\"refresh_token\":7,\"expires_in\":60}",
        "{\"access_token\":\"a\",\"refresh_token\":\"r\",\"expires_in\":60}junk"
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++)
    {
        char *access = NULL;
        char *refresh = NULL;
        char *account = NULL;
        char *plan = NULL;
        time_t expires_at = 0;
        ck_assert_int_eq(openai_oauth_test_parse_token(invalid[index], 0, 100,
            &access, &refresh, &account, &plan, &expires_at), -1);
        ck_assert_ptr_null(access);
        ck_assert_ptr_null(refresh);
    }
}
END_TEST

START_TEST(test_jwt_parser_rejects_malformed_segments)
{
    const char *invalid[] = {
        NULL, "", "one", "one.two", "one..three", "one.%%%%.three",
        "one.e30.three.extra", "one.e30."
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++)
    {
        char *account = NULL;
        char *plan = NULL;
        ck_assert_int_eq(openai_oauth_test_jwt_metadata(invalid[index],
                                                       &account, &plan), -1);
        ck_assert_ptr_null(account);
        ck_assert_ptr_null(plan);
    }
}
END_TEST

START_TEST(test_refresh_expiry_boundary_includes_five_minute_skew)
{
    ck_assert_int_eq(openai_oauth_test_needs_refresh(1300, 1000), 1);
    ck_assert_int_eq(openai_oauth_test_needs_refresh(1301, 1000), 0);
    ck_assert_int_eq(openai_oauth_test_needs_refresh(999, 1000), 1);
}
END_TEST

START_TEST(test_device_responses_require_complete_exact_fields)
{
    char *device_auth_id = NULL;
    char *user_code = NULL;
    unsigned int interval = 0;
    unsigned int expires_in = 0;
    const char start[] =
        "{\"device_auth_id\":\"device-1\",\"user_code\":\"ABCD-EFGH\","
        "\"interval\":5,\"expires_in\":900}";
    ck_assert_int_eq(openai_oauth_test_parse_device_start(start, &device_auth_id,
        &user_code, &interval, &expires_in), 0);
    ck_assert_str_eq(device_auth_id, "device-1");
    ck_assert_str_eq(user_code, "ABCD-EFGH");
    ck_assert_uint_eq(interval, 5U);
    ck_assert_uint_eq(expires_in, 900U);
    free(device_auth_id);
    free(user_code);

    char *code = NULL;
    char *verifier = NULL;
    const char token[] =
        "{\"authorization_code\":\"code-1\",\"code_verifier\":\"verify-1\"}";
    ck_assert_int_eq(openai_oauth_test_parse_device_authorization(token,
        &code, &verifier), 0);
    ck_assert_str_eq(code, "code-1");
    ck_assert_str_eq(verifier, "verify-1");
    free(code);
    free(verifier);

    const char invalid[] =
        "{\"device_auth_id\":\"device-1\",\"user_code\":\"ABCD\",\"interval\":0}";
    ck_assert_int_eq(openai_oauth_test_parse_device_start(invalid, &device_auth_id,
        &user_code, &interval, &expires_in), -1);
    ck_assert_ptr_null(device_auth_id);
    ck_assert_ptr_null(user_code);
}
END_TEST

START_TEST(test_cancel_login_requires_matching_id_and_stops_listener)
{
    OpenAIOAuth *auth = openai_oauth_create();
    ck_assert_ptr_nonnull(auth);
    ck_assert_int_eq(openai_oauth_attach_session(auth, (SessionManager *)1), 0);
    char *authorization_url = NULL;
    char *login_id = NULL;
    ck_assert_int_eq(openai_oauth_start(auth, &authorization_url, &login_id), 0);
    ck_assert_ptr_nonnull(authorization_url);
    ck_assert_ptr_nonnull(login_id);
    ck_assert_int_eq(openai_oauth_cancel_login(auth, "wrong-login-id"), -1);
    ck_assert_int_eq(openai_oauth_cancel_login(auth, login_id), 0);
    ck_assert_int_eq(openai_oauth_status(auth, NULL, NULL, NULL),
                     OPENAI_OAUTH_SIGNED_OUT);
    free(authorization_url);
    free(login_id);
    openai_oauth_destroy(auth);
}
END_TEST

static Suite *openai_oauth_suite(void)
{
    Suite *suite = suite_create("openai_oauth");
    TCase *url = tcase_create("url_pkce");
    TCase *callback = tcase_create("callback_parser");
    TCase *token = tcase_create("token_jwt_expiry");
    TCase *lifecycle = tcase_create("lifecycle");
    tcase_add_test(url, test_pkce_matches_rfc7636_s256_vector);
    tcase_add_test(url, test_authorize_url_contains_encoded_required_parameters);
    tcase_add_test(callback, test_callback_parser_accepts_exact_success_request);
    tcase_add_test(callback, test_callback_parser_accepts_denial_as_error_state);
    tcase_add_test(callback, test_callback_parser_rejects_duplicate_and_malformed_fields);
    tcase_add_test(callback, test_callback_parser_rejects_wrong_method_path_and_smuggling);
    tcase_add_test(callback, test_callback_parser_rejects_embedded_nul_and_oversize);
    tcase_add_test(token, test_token_parser_validates_and_extracts_jwt_metadata);
    tcase_add_test(token, test_token_parser_reuses_refresh_token_only_for_refresh_response);
    tcase_add_test(token, test_token_parser_rejects_missing_invalid_and_trailing_data);
    tcase_add_test(token, test_jwt_parser_rejects_malformed_segments);
    tcase_add_test(token, test_refresh_expiry_boundary_includes_five_minute_skew);
    tcase_add_test(token, test_device_responses_require_complete_exact_fields);
    tcase_set_timeout(lifecycle, 5);
    tcase_add_test(lifecycle,
                   test_cancel_login_requires_matching_id_and_stops_listener);
    tcase_add_test(lifecycle,
                   test_logout_keeps_credentials_when_durable_delete_fails);
    suite_add_tcase(suite, url);
    suite_add_tcase(suite, callback);
    suite_add_tcase(suite, token);
    suite_add_tcase(suite, lifecycle);
    return suite;
}

int main(void)
{
    Suite *suite = openai_oauth_suite();
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
