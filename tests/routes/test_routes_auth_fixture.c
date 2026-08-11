/*
 * test_routes_auth_fixture.c - shared stubs and fixtures for the
 * routes_auth test binaries (auth/auth_password): encryption,
 * middleware, rate-limiter, and session stubs plus capture state.
 * Split from test_routes_auth.c (2026-08 file-length compliance).
 * Depends on: check, routes_auth.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "test_routes_auth_fixture.h"

/* test_routes_auth - unit tests for routes auth. Depends on: check, the module under test. */
extern int openai_oauth_stub_attach_result;

/* ---------------------------------------------------------------------------
 * Stub state
 * --------------------------------------------------------------------------- */

int stub_rate_allow = 1;
int stub_unlock_result = 1;
int stub_has_valid_token_result = 1;
int stub_salt_result = 0;
int stub_key_derive_result = 0;
int stub_encrypt_check_verifier_result = 0;
int stub_migration_change_result = 0;
int stub_migration_call_count = 0;
int stub_rate_failure_count = 0;
SessionManager *stub_sm_create_result = NULL;
int captured_status = 0;
char *captured_body = NULL;

void reset_capture(void)
{
    captured_status = 0;
    free(captured_body);
    captured_body = NULL;
}

void reset_stubs(void)
{
    stub_rate_allow = 1;
    stub_unlock_result = 1;
    stub_has_valid_token_result = 1;
    stub_salt_result = 0;
    stub_key_derive_result = 0;
    stub_encrypt_check_verifier_result = 0;
    stub_migration_change_result = 0;
    stub_migration_call_count = 0;
    stub_rate_failure_count = 0;
    stub_sm_create_result = NULL;
    openai_oauth_stub_attach_result = 0;
    reset_capture();
}

/* ---------------------------------------------------------------------------
 * Stub server functions
 * --------------------------------------------------------------------------- */

int server_response(Client *client, int status, const char *content_type,
                     const char *body)
{
    (void)client; (void)content_type;
    captured_status = status;
    free(captured_body);
    captured_body = body ? str_dup(body) : NULL;
    return 0;
}

int server_response_json(Client *client, int status, const char *json)
{
    (void)client;
    captured_status = status;
    free(captured_body);
    captured_body = json ? str_dup(json) : NULL;
    return 0;
}

int server_response_error(Client *client, int status, const char *msg)
{
    (void)client;
    captured_status = status;
    free(captured_body);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg ? msg : "");
    captured_body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return 0;
}

void client_close(Client *client) { (void)client; }
int server_sse_write(Client *client, const char *data)
 {
    (void)client;
    (void)data;
    return 0;
}
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
    return stub_has_valid_token_result;
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
    stub_rate_failure_count++;
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
    stub_migration_call_count++;
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
 {
    (void)level;
    (void)file;
    (void)line;
    (void)fmt;
}
void setup(void) { reset_stubs(); }
void teardown(void) { reset_stubs(); }
