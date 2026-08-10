#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server/routes/routes_openai_auth.h"
#include "llm/openai_oauth.h"

static int captured_status = 0;
static char *captured_body = NULL;
static OpenAIOAuthState stub_state = OPENAI_OAUTH_SIGNED_OUT;
static int stub_status_for_login_result = 0;
static int stub_start_result = 0;
static int stub_logout_result = 0;
static int stub_cancel_result = 0;
static int start_calls = 0;
static int logout_calls = 0;
static int cancel_calls = 0;
static char captured_login_id[129] = {0};

static char *copy_string(const char *value)
{
    size_t length = strlen(value) + 1;
    char *copy = malloc(length);
    if (copy) memcpy(copy, value, length);
    return copy;
}

int server_response_json(Client *client, int status, const char *json)
{
    (void)client;
    captured_status = status;
    free(captured_body);
    captured_body = copy_string(json ? json : "");
    return 0;
}

int server_response_error(Client *client, int status, const char *message)
{
    (void)client;
    captured_status = status;
    free(captured_body);
    captured_body = copy_string(message ? message : "");
    return 0;
}

OpenAIOAuthState openai_oauth_status(OpenAIOAuth *auth, char **account_id,
                                     char **plan_type, char **error)
{
    (void)auth;
    if (account_id) *account_id = copy_string("account-public");
    if (plan_type) *plan_type = copy_string("plus");
    if (error) *error = NULL;
    return stub_state;
}

int openai_oauth_status_for_login(OpenAIOAuth *auth, const char *login_id,
                                  OpenAIOAuthState *state, char **account_id,
                                  char **plan_type, char **error)
{
    (void)auth;
    if (stub_status_for_login_result != 0) return -1;
    int written = snprintf(captured_login_id, sizeof(captured_login_id), "%s",
                           login_id ? login_id : "");
    if (written < 0 || (size_t)written >= sizeof(captured_login_id)) return -1;
    *state = stub_state;
    if (account_id) *account_id = NULL;
    if (plan_type) *plan_type = NULL;
    if (error) *error = NULL;
    return 0;
}

int openai_oauth_start(OpenAIOAuth *auth, char **authorization_url,
                       char **login_id)
{
    (void)auth;
    start_calls++;
    *authorization_url = NULL;
    *login_id = NULL;
    if (stub_start_result != 0) return -1;
    *authorization_url = copy_string("https://auth.openai.com/authorize");
    *login_id = copy_string("login_123");
    return *authorization_url && *login_id ? 0 : -1;
}

int openai_oauth_cancel_login(OpenAIOAuth *auth, const char *login_id)
{
    (void)auth;
    cancel_calls++;
    int written = snprintf(captured_login_id, sizeof(captured_login_id), "%s",
                           login_id ? login_id : "");
    return written < 0 || (size_t)written >= sizeof(captured_login_id) ? -1 :
           stub_cancel_result;
}

int openai_oauth_logout(OpenAIOAuth *auth)
{
    (void)auth;
    logout_calls++;
    return stub_logout_result;
}

static void setup(void)
{
    captured_status = 0;
    free(captured_body);
    captured_body = NULL;
    stub_state = OPENAI_OAUTH_SIGNED_OUT;
    stub_status_for_login_result = 0;
    stub_start_result = 0;
    stub_logout_result = 0;
    stub_cancel_result = 0;
    start_calls = 0;
    logout_calls = 0;
    cancel_calls = 0;
    captured_login_id[0] = '\0';
}

static void teardown(void)
{
    free(captured_body);
    captured_body = NULL;
}

START_TEST(test_status_returns_public_fields_only)
{
    HTTPRequest request = {0};
    ServerContext context = {.openai_oauth = (OpenAIOAuth *)1};
    stub_state = OPENAI_OAUTH_SIGNED_IN;
    handle_openai_oauth_status(&request, NULL, &context);
    ck_assert_int_eq(captured_status, 200);
    ck_assert_ptr_nonnull(strstr(captured_body, "\"state\":\"signed_in\""));
    ck_assert_ptr_nonnull(strstr(captured_body, "account-public"));
    ck_assert_ptr_null(strstr(captured_body, "access_token"));
    ck_assert_ptr_null(strstr(captured_body, "refresh_token"));
}
END_TEST

START_TEST(test_status_validates_login_id)
{
    HTTPRequest request = {0};
    ServerContext context = {.openai_oauth = (OpenAIOAuth *)1};
    memcpy(request.query, "login_id=abc_123-XYZ", sizeof("login_id=abc_123-XYZ"));
    stub_state = OPENAI_OAUTH_PENDING;
    handle_openai_oauth_status(&request, NULL, &context);
    ck_assert_int_eq(captured_status, 200);
    ck_assert_str_eq(captured_login_id, "abc_123-XYZ");

    setup();
    memcpy(request.query, "login_id=unknown", sizeof("login_id=unknown"));
    stub_status_for_login_result = -1;
    handle_openai_oauth_status(&request, NULL, &context);
    ck_assert_int_eq(captured_status, 404);
}
END_TEST

START_TEST(test_status_rejects_malformed_login_id_query)
{
    HTTPRequest request = {0};
    ServerContext context = {.openai_oauth = (OpenAIOAuth *)1};
    memcpy(request.query, "login_id=one&login_id=two",
           sizeof("login_id=one&login_id=two"));
    handle_openai_oauth_status(&request, NULL, &context);
    ck_assert_int_eq(captured_status, 400);
}
END_TEST

START_TEST(test_start_requires_unlocked_storage)
{
    HTTPRequest request = {0};
    ServerContext context = {.openai_oauth = (OpenAIOAuth *)1};
    handle_openai_oauth_start(&request, NULL, &context);
    ck_assert_int_eq(captured_status, 503);
    ck_assert_int_eq(start_calls, 0);
}
END_TEST

START_TEST(test_start_returns_url_and_opaque_login_id)
{
    HTTPRequest request = {0};
    ServerContext context = {.openai_oauth = (OpenAIOAuth *)1,
                             .sm = (SessionManager *)1};
    handle_openai_oauth_start(&request, NULL, &context);
    ck_assert_int_eq(captured_status, 200);
    ck_assert_int_eq(start_calls, 1);
    ck_assert_ptr_nonnull(strstr(captured_body, "authorization_url"));
    ck_assert_ptr_nonnull(strstr(captured_body, "login_123"));
}
END_TEST

START_TEST(test_logout_with_login_id_only_cancels_attempt)
{
    char body[] = "{\"login_id\":\"login_123\"}";
    HTTPRequest request = {.body = body, .body_len = sizeof(body) - 1};
    ServerContext context = {.openai_oauth = (OpenAIOAuth *)1};
    handle_openai_oauth_logout(&request, NULL, &context);
    ck_assert_int_eq(captured_status, 200);
    ck_assert_int_eq(cancel_calls, 1);
    ck_assert_int_eq(logout_calls, 0);
    ck_assert_str_eq(captured_login_id, "login_123");
}
END_TEST

START_TEST(test_logout_without_login_id_removes_credentials)
{
    HTTPRequest request = {0};
    ServerContext context = {.openai_oauth = (OpenAIOAuth *)1};
    handle_openai_oauth_logout(&request, NULL, &context);
    ck_assert_int_eq(captured_status, 200);
    ck_assert_int_eq(cancel_calls, 0);
    ck_assert_int_eq(logout_calls, 1);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("OpenAI OAuth Routes");
    TCase *status = tcase_create("Status");
    tcase_add_checked_fixture(status, setup, teardown);
    tcase_add_test(status, test_status_returns_public_fields_only);
    tcase_add_test(status, test_status_validates_login_id);
    tcase_add_test(status, test_status_rejects_malformed_login_id_query);
    suite_add_tcase(suite, status);

    TCase *lifecycle = tcase_create("Lifecycle");
    tcase_add_checked_fixture(lifecycle, setup, teardown);
    tcase_set_timeout(lifecycle, 5);
    tcase_add_test(lifecycle, test_start_requires_unlocked_storage);
    tcase_add_test(lifecycle, test_start_returns_url_and_opaque_login_id);
    tcase_add_test(lifecycle, test_logout_with_login_id_only_cancels_attempt);
    tcase_add_test(lifecycle, test_logout_without_login_id_removes_credentials);
    suite_add_tcase(suite, lifecycle);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
