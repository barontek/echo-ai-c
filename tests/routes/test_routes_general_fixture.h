/*
 * test_routes_general_fixture.h - shared stub state for the
 * routes_general test binaries.
 */

#ifndef ECHO_TEST_ROUTES_GENERAL_FIXTURE_H
#define ECHO_TEST_ROUTES_GENERAL_FIXTURE_H

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <cjson/cJSON.h>

/* The test TU defines its own curl stubs (shadowing libcurl at link
 * time), so it must not include the real curl.h; these minimal types
 * keep the stub prototypes consistent with the call sites. */
typedef void CURL;
typedef int CURLcode;
enum { CURLE_OK = 0, CURLE_UNKNOWN_OPTION = 1, CURLE_URL_MALFORMAT = 3,
       CURLOPT_URL = 10002, CURLOPT_TIMEOUT = 13,
       CURLOPT_WRITEFUNCTION = 20011, CURLOPT_WRITEDATA = 10001,
       CURLOPT_HTTPHEADER = 10023 };
struct curl_slist {
    char *data;
    struct curl_slist *next;
};

#include "../src/server/routes/routes.h"
#include "../src/server/routes/routes_general.h"
#include "../src/llm/openai.h"
#include "../src/config/config.h"
#include "../src/utils/string_utils.h"

extern OpenAIOAuthState openai_oauth_stub_state;
extern int dummy_metrics_ptr;
extern int stub_has_valid_token;
extern int stub_ct_undo_result;
extern int stub_ct_redo_result;
extern char *stub_metrics_body;
extern int stub_curl_init_nonnull;
extern int stub_curl_perform_code;
extern void *captured_writedata;
extern size_t (*captured_writefunc)(void *, size_t, size_t, void *);
extern const char *stub_models_json;
extern char captured_curl_url[256];
extern char captured_auth_header[256];
extern int captured_status;
extern char *captured_body;
extern int stub_openai_models_result;
extern const char *stub_openai_models[8];
extern size_t stub_openai_models_count;

void reset_capture(void);
void reset_stubs(void);
void setup(void);
void teardown(void);

#endif /* ECHO_TEST_ROUTES_GENERAL_FIXTURE_H */
