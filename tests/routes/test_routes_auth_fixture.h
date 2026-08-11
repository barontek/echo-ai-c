/*
 * test_routes_auth_fixture.h - shared stub state for the routes_auth
 * test binaries.
 */

#ifndef ECHO_TEST_ROUTES_AUTH_FIXTURE_H
#define ECHO_TEST_ROUTES_AUTH_FIXTURE_H

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "../src/server/routes/routes.h"
#include "../src/server/routes/routes_auth.h"
#include "../src/utils/string_utils.h"

extern int openai_oauth_stub_attach_result;
extern int stub_rate_allow;
extern int stub_unlock_result;
extern int stub_has_valid_token_result;
extern int stub_salt_result;
extern int stub_key_derive_result;
extern int stub_encrypt_check_verifier_result;
extern int stub_migration_change_result;
extern int stub_migration_call_count;
extern int stub_rate_failure_count;
extern SessionManager *stub_sm_create_result;
extern int captured_status;
extern char *captured_body;

void reset_capture(void);
void reset_stubs(void);
void setup(void);
void teardown(void);

#endif /* ECHO_TEST_ROUTES_AUTH_FIXTURE_H */
