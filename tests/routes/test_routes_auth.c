#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "../src/server/routes/routes.h"
#include "../src/server/routes/routes_auth.h"
#include "../src/utils/string_utils.h"

extern int openai_oauth_stub_attach_result;

/* ---------------------------------------------------------------------------
 * Stub state
 * --------------------------------------------------------------------------- */

static int stub_rate_allow = 1;
static int stub_unlock_result = 1;
static int stub_salt_result = 0;
static int stub_key_derive_result = 0;
static int stub_encrypt_check_verifier_result = 0;
static int stub_migration_change_result = 0;
static SessionManager *stub_sm_create_result = NULL;
static int captured_status = 0;
static char *captured_body = NULL;

static void reset_capture(void)
{
    captured_status = 0;
    free(captured_body);
    captured_body = NULL;
}

static void reset_stubs(void)
{
    stub_rate_allow = 1;
    stub_unlock_result = 1;
    stub_salt_result = 0;
    stub_key_derive_result = 0;
    stub_encrypt_check_verifier_result = 0;
    stub_migration_change_result = 0;
    stub_sm_create_result = NULL;
    openai_oauth_stub_attach_result = 0;
    reset_capture();
}

/* ---------------------------------------------------------------------------
 * Stub server functions
 * --------------------------------------------------------------------------- */

void server_response(Client *client, int status, const char *content_type,
                     const char *body)
{
    (void)client; (void)content_type;
    captured_status = status;
    free(captured_body);
    captured_body = body ? str_dup(body) : NULL;
}

void server_response_json(Client *client, int status, const char *json)
{
    (void)client;
    captured_status = status;
    free(captured_body);
    captured_body = json ? str_dup(json) : NULL;
}

void server_response_error(Client *client, int status, const char *msg)
{
    (void)client;
    captured_status = status;
    free(captured_body);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg ? msg : "");
    captured_body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
}

void client_close(Client *client) { (void)client; }
void server_sse_write(Client *client, const char *data)
{ (void)client; (void)data; }
void routes_ws_invalidate_auth(ServerContext *ctx) { (void)ctx; }

/* ---------------------------------------------------------------------------
 * Stub middleware
 * --------------------------------------------------------------------------- */

int middleware_check_unlock(HTTPRequest *req, ServerContext *ctx)
{
    (void)req; (void)ctx;
    return stub_unlock_result;
}

int middleware_has_valid_token(const char *headers, const char *token)
{
    (void)headers; (void)token;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Stub rate_limiter
 * --------------------------------------------------------------------------- */

int rate_limiter_unlock_allowed(RateLimiter *rl, const char *ip, int per_ip, int global)
{
    (void)rl; (void)ip; (void)per_ip; (void)global;
    return stub_rate_allow;
}

void rate_limiter_record_unlock_failure(RateLimiter *rl, const char *ip)
{
    (void)rl; (void)ip;
}

/* ---------------------------------------------------------------------------
 * Stub encryption
 * --------------------------------------------------------------------------- */

int encryption_salt_load(const char *path, unsigned char *salt, int *salt_len)
{
    (void)path; (void)salt; (void)salt_len;
    return stub_salt_result;
}

int encryption_key_derive(const char *password, const unsigned char *salt,
                           int salt_len, EncryptionKey *key)
{
    (void)password; (void)salt; (void)salt_len; (void)key;
    return stub_key_derive_result;
}

int encryption_check_verifier(const EncryptionKey *key, const char *path)
{
    (void)key; (void)path;
    return stub_encrypt_check_verifier_result;
}

/* ---------------------------------------------------------------------------
 * Stub session_manager / migration
 * --------------------------------------------------------------------------- */

SessionManager *session_manager_create(const char *dir, const char *pw)
{
    (void)dir; (void)pw;
    return stub_sm_create_result;
}

SessionManager *session_manager_create_ex(const char *dir, const char *pw,
                                          SessionManagerCreateResult *result)
{
    (void)dir; (void)pw;
    if (result)
    {
        if (stub_sm_create_result)
            *result = SESSION_MANAGER_CREATE_OK;
        else if (stub_encrypt_check_verifier_result != 0)
            *result = SESSION_MANAGER_CREATE_AUTH_FAILED;
        else
            *result = SESSION_MANAGER_CREATE_STORAGE_FAILED;
    }
    return stub_sm_create_result;
}

void session_manager_free(SessionManager *sm) { (void)sm; }

int migration_change_password(SessionManager *sm, const char *new_pw)
{
    (void)sm; (void)new_pw;
    return stub_migration_change_result;
}

/* ---------------------------------------------------------------------------
 * Stub registry / agent
 * --------------------------------------------------------------------------- */

void registry_set_session_manager(SessionManager *sm) { (void)sm; }
void agent_set_session_manager(Agent *a, SessionManager *sm) { (void)a; (void)sm; }

/* ---------------------------------------------------------------------------
 * Stub logging
 * --------------------------------------------------------------------------- */

void log_init(void) {}
void log_cleanup(void) {}
void log_set_level(int l) { (void)l; }
void log_msg(int level, const char *file, int line, const char *fmt, ...)
{ (void)level; (void)file; (void)line; (void)fmt; }

/* ---------------------------------------------------------------------------
 * handle_setup
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_setup_wrong_state)
{
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    HTTPRequest req = {0};

    handle_setup(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_setup_no_body)
{
    ServerContext ctx = {0};
    ctx.state = STATE_SETUP;
    HTTPRequest req = {0};

    handle_setup(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_setup_invalid_json)
{
    ServerContext ctx = {0};
    ctx.state = STATE_SETUP;
    HTTPRequest req = {0};
    req.body = str_dup("not json");
    req.body_len = strlen(req.body);

    handle_setup(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_setup_short_password)
{
    ServerContext ctx = {0};
    ctx.state = STATE_SETUP;
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"ab\"}");
    req.body_len = strlen(req.body);

    handle_setup(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_setup_sm_create_fails)
{
    stub_sm_create_result = NULL;

    ServerContext ctx = {0};
    ctx.state = STATE_SETUP;
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"goodpassword\"}");
    req.body_len = strlen(req.body);

    handle_setup(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_setup_success)
{
    SessionManager sm = {0};
    stub_sm_create_result = &sm;

    ServerContext ctx = {0};
    ctx.state = STATE_SETUP;
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"goodpassword\"}");
    req.body_len = strlen(req.body);

    handle_setup(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(captured_body && strstr(captured_body, "\"token\":"));
    ck_assert(ctx.state == STATE_UNLOCKED);
    ck_assert_ptr_nonnull(ctx.unlock_token);
    ck_assert_uint_eq(strlen(ctx.unlock_token), 68);
    ck_assert_int_eq(strncmp(ctx.unlock_token, "tok_", 4), 0);

    free(ctx.unlock_token);
    ctx.unlock_token = NULL;
    free(req.body);
    reset_stubs();
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_unlock
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_unlock_wrong_state)
{
    ServerContext ctx = {0};
    ctx.state = STATE_SETUP;
    HTTPRequest req = {0};

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_rate_limited)
{
    stub_rate_allow = 0;

    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    ctx.rate_limiter = (RateLimiter *)&ctx; /* non-NULL trigger */
    HTTPRequest req = {0};

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 429);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_no_body)
{
    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_invalid_json)
{
    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};
    req.body = str_dup("bad");
    req.body_len = strlen(req.body);

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_missing_password)
{
    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};
    req.body = str_dup("{}");
    req.body_len = strlen(req.body);

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_salt_not_found)
{
    stub_salt_result = -1;

    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"test\"}");
    req.body_len = strlen(req.body);

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_key_derive_fails)
{
    stub_salt_result = 0;
    stub_key_derive_result = -1;

    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"test\"}");
    req.body_len = strlen(req.body);

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_encrypt_check_verifier_fails)
{
    stub_encrypt_check_verifier_result = -1;

    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    ctx.rate_limiter = (void *)&ctx;
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"test\"}");
    req.body_len = strlen(req.body);

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 401);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_unlock_success)
{
    SessionManager manager = {0};
    stub_sm_create_result = &manager;
    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};
    req.body = str_dup("{\"password\":\"goodpassword\"}");
    req.body_len = strlen(req.body);

    handle_unlock(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(captured_body && strstr(captured_body, "\"token\":"));
    ck_assert(ctx.state == STATE_UNLOCKED);
    ck_assert_ptr_nonnull(ctx.unlock_token);
    ck_assert_uint_eq(strlen(ctx.unlock_token), 68);
    ck_assert_int_eq(strncmp(ctx.unlock_token, "tok_", 4), 0);

    free(ctx.unlock_token);
    ctx.unlock_token = NULL;
    free(req.body);
    reset_stubs();
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_logout
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_logout_unauthorized)
{
    stub_unlock_result = 0;

    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_logout(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 401);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_logout_success)
{
    ServerContext ctx = {0};
    ctx.unlock_token = str_dup("token123");
    ctx.state = STATE_UNLOCKED;
    HTTPRequest req = {0};

    handle_logout(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "logged out"));
    ck_assert_ptr_null(ctx.unlock_token);
    ck_assert_int_eq(ctx.state, STATE_LOCKED);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_logout_preserves_unlock_state_when_detach_fails)
{
    ServerContext ctx = {0};
    ctx.unlock_token = str_dup("token123");
    ctx.state = STATE_UNLOCKED;
    ctx.openai_oauth = (OpenAIOAuth *)&ctx;
    HTTPRequest req = {0};
    openai_oauth_stub_attach_result = -1;

    handle_logout(&req, NULL, &ctx);

    ck_assert_int_eq(captured_status, 500);
    ck_assert_ptr_nonnull(ctx.unlock_token);
    ck_assert_int_eq(ctx.state, STATE_UNLOCKED);
    openai_oauth_stub_attach_result = 0;
    free(ctx.unlock_token);
    reset_stubs();
}
END_TEST

/* ---------------------------------------------------------------------------
 * handle_change_password
 * --------------------------------------------------------------------------- */

START_TEST(test_handle_change_password_no_sm)
{
    ServerContext ctx = {0};
    ctx.sm = NULL;
    HTTPRequest req = {0};

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_no_body)
{
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_invalid_json)
{
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("bad");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_short)
{
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("{\"new_password\":\"x\"}");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_migration_fails)
{
    stub_migration_change_result = -1;
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("{\"new_password\":\"goodpw\"}");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_reports_committed_recovery_state)
{
    stub_migration_change_result = -2;
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("{\"new_password\":\"goodpw\"}");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);
    ck_assert_ptr_nonnull(strstr(captured_body, "restart"));
    ck_assert_ptr_nonnull(strstr(captured_body, "new password"));

    free(req.body);
    reset_stubs();
}
END_TEST

START_TEST(test_handle_change_password_success)
{
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.sm = &sm;
    HTTPRequest req = {0};
    req.body = str_dup("{\"new_password\":\"goodpw\"}");
    req.body_len = strlen(req.body);

    handle_change_password(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"changed\":true"));

    free(req.body);
    reset_stubs();
}
END_TEST

/* ---------------------------------------------------------------------------
 * Suite
 * --------------------------------------------------------------------------- */

static void setup(void) { reset_stubs(); }
static void teardown(void) { reset_stubs(); }

Suite *routes_auth_suite(void)
{
    Suite *s = suite_create("routes_auth");

    TCase *tc = tcase_create("handle_setup");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_setup_wrong_state);
    tcase_add_test(tc, test_handle_setup_no_body);
    tcase_add_test(tc, test_handle_setup_invalid_json);
    tcase_add_test(tc, test_handle_setup_short_password);
    tcase_add_test(tc, test_handle_setup_sm_create_fails);
    tcase_add_test(tc, test_handle_setup_success);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_unlock");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_unlock_wrong_state);
    tcase_add_test(tc, test_handle_unlock_rate_limited);
    tcase_add_test(tc, test_handle_unlock_no_body);
    tcase_add_test(tc, test_handle_unlock_invalid_json);
    tcase_add_test(tc, test_handle_unlock_missing_password);
    tcase_add_test(tc, test_handle_unlock_salt_not_found);
    tcase_add_test(tc, test_handle_unlock_key_derive_fails);
    tcase_add_test(tc, test_handle_unlock_encrypt_check_verifier_fails);
    tcase_add_test(tc, test_handle_unlock_success);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_logout");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_logout_unauthorized);
    tcase_add_test(tc, test_handle_logout_success);
    tcase_add_test(tc, test_handle_logout_preserves_unlock_state_when_detach_fails);
    suite_add_tcase(s, tc);

    tc = tcase_create("handle_change_password");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_change_password_no_sm);
    tcase_add_test(tc, test_handle_change_password_no_body);
    tcase_add_test(tc, test_handle_change_password_invalid_json);
    tcase_add_test(tc, test_handle_change_password_short);
    tcase_add_test(tc, test_handle_change_password_migration_fails);
    tcase_add_test(tc,
                   test_handle_change_password_reports_committed_recovery_state);
    tcase_add_test(tc, test_handle_change_password_success);
    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    int failures = 0;
    Suite *s = routes_auth_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_ENV);
    failures = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failures;
}
