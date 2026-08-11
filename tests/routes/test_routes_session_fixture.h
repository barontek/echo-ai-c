/*
 * test_routes_session_fixture.h - shared stub state and context
 * builders for the routes_session test binaries.
 */

#ifndef ECHO_TEST_ROUTES_SESSION_FIXTURE_H
#define ECHO_TEST_ROUTES_SESSION_FIXTURE_H

#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "../src/server/routes/routes.h"
#include "../src/server/routes/routes_session.h"
#include "../src/session/session_branch.h"
#include "../src/utils/string_utils.h"

extern int stub_unlock_result;
extern int stub_list_result_null;
extern int stub_list_count;
extern char *stub_list_ids[4];
extern char *stub_list_titles[4];
extern char *stub_list_created_ats[4];
extern int stub_list_title_gens[4];
extern Session *stub_create_result;
extern Session *stub_load_result;
extern int stub_save_result;
extern int stub_delete_result;
extern int stub_import_result_null;
extern char *stub_export_result;
extern const char *stub_branch_info_json;
extern int captured_status;
extern char *captured_body;

void reset_capture(void);
void reset_stubs(void);
void setup(void);
void teardown(void);
ServerContext make_ctx(SessionManager *sm, ServerState state,
                       const char *token);
HTTPRequest make_req(const char *path, const char *body,
                     const char *headers);
void free_req(HTTPRequest *req);

/* Defined in routes_session.c under ROUTES_SESSION_ALLOC_TEST. */
void routes_session_test_set_alloc_fail(int nth_allocation);

#endif /* ECHO_TEST_ROUTES_SESSION_FIXTURE_H */
