/* test_routes_general_general.c - routes general handle_general tests
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

START_TEST(test_handle_health_ok)
{
    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_health(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"status\":\"ok\""));

    reset_stubs();
}

START_TEST(test_handle_providers_lists_available_providers)
{
    ServerContext ctx = {0};
    HTTPRequest req = {0};

    handle_providers(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"providers\":["));
    ck_assert(strstr(captured_body, "\"opencode_zen\""));
    ck_assert(strstr(captured_body, "\"ollama\""));
    ck_assert(strstr(captured_body, "\"effort_supported\":[\"ollama\",\"openai\",\"openai_compatible\",\"opencode_zen\"]"));
    ck_assert(strstr(captured_body, "\"effort_options\":{\"ollama\":[\"low\",\"medium\",\"high\",\"max\",\"none\"],\"openai\":[\"low\",\"medium\",\"high\",\"xhigh\",\"max\",\"none\"],\"openai_compatible\":[\"low\",\"medium\",\"high\",\"max\",\"none\"],\"opencode_zen\":[\"low\",\"medium\",\"high\",\"max\",\"none\"]}"));

    reset_stubs();
}

START_TEST(test_handle_status_locked)
{
    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};

    handle_status(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"locked\":true"));
    ck_assert(strstr(captured_body, "\"needs_setup\":false"));

    reset_stubs();
}

START_TEST(test_handle_status_setup)
{
    ServerContext ctx = {0};
    ctx.state = STATE_SETUP;
    HTTPRequest req = {0};

    handle_status(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"locked\":false"));
    ck_assert(strstr(captured_body, "\"needs_setup\":true"));

    reset_stubs();
}

START_TEST(test_handle_status_unlocked_valid_token)
{
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = "secret";
    stub_has_valid_token = 1;
    HTTPRequest req = {0};

    handle_status(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"locked\":false"));

    reset_stubs();
}

START_TEST(test_handle_status_unlocked_bad_token)
{
    /* token is set but middleware rejects it → locked=1 */
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = "secret";
    stub_has_valid_token = 0;
    HTTPRequest req = {0};

    handle_status(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"locked\":true"));

    reset_stubs();
}

START_TEST(test_handle_status_noop_token_unlocked_without_header)
{
    /* session-management-disabled mode ("noop" token): the FE must never
     * see locked=1, or it is stuck on the unlock screen forever. */
    ServerContext ctx = {0};
    ctx.state = STATE_UNLOCKED;
    ctx.unlock_token = "noop";
    stub_has_valid_token = 0;
    HTTPRequest req = {0};

    handle_status(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"locked\":false"));
    ck_assert(strstr(captured_body, "\"needs_setup\":false"));

    reset_stubs();
}

START_TEST(test_handle_status_session_enabled)
{
    SessionManager sm = {0};
    ServerContext ctx = {0};
    ctx.state = STATE_LOCKED;
    ctx.sm = &sm;
    HTTPRequest req = {0};

    handle_status(&req, NULL, &ctx);
    ck_assert(strstr(captured_body, "\"session_enabled\":true"));

    reset_stubs();
}

START_TEST(test_handle_config_no_agent)
{
    ServerContext ctx = {0};
    ctx.agent = NULL;
    HTTPRequest req = {0};

    handle_config(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"provider\":\"ollama\""));
    ck_assert(strstr(captured_body, "\"temperature\":0.7"));
    ck_assert(strstr(captured_body, "\"max_iterations\":50"));

    reset_stubs();
}

START_TEST(test_handle_config_with_agent)
{
    Agent agent = {0};
    agent.model = "deepseek";
    agent.temperature = 0.3;
    agent.max_iterations = 25;

    ServerContext ctx = {0};
    ctx.agent = &agent;
    HTTPRequest req = {0};

    handle_config(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"provider\":\"ollama\""));
    ck_assert(strstr(captured_body, "\"model\":\"deepseek\""));
    ck_assert(strstr(captured_body, "\"temperature\":0.3"));
    ck_assert(strstr(captured_body, "\"max_iterations\":25"));

    reset_stubs();
}

START_TEST(test_handle_metrics_no_metrics)
{
    ServerContext ctx = {0};
    ctx.metrics = NULL;
    HTTPRequest req = {0};

    handle_metrics(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    reset_stubs();
}

START_TEST(test_handle_metrics_success)
{
    stub_metrics_body = "echo_requests 42";

    ServerContext ctx = {0};
    ctx.metrics = (Metrics *)&dummy_metrics_ptr;
    HTTPRequest req = {0};

    handle_metrics(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "echo_requests 42"));

    reset_stubs();
}

START_TEST(test_handle_metrics_render_null)
{
    stub_metrics_body = NULL;

    ServerContext ctx = {0};
    ctx.metrics = (Metrics *)&dummy_metrics_ptr;
    HTTPRequest req = {0};

    handle_metrics(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 500);

    reset_stubs();
}

START_TEST(test_handle_undo_no_tracker)
{
    ServerContext ctx = {0};
    ctx.change_tracker = NULL;
    HTTPRequest req = {0};

    handle_undo(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}

START_TEST(test_handle_undo_nothing)
{
    ChangeTracker ct = {0};
    stub_ct_undo_result = -1;

    ServerContext ctx = {0};
    ctx.change_tracker = &ct;
    HTTPRequest req = {0};

    handle_undo(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"undo\":false"));

    reset_stubs();
}

START_TEST(test_handle_undo_success)
{
    int dummy_ct = 0;
    stub_ct_undo_result = 1024;

    ServerContext ctx = {0};
    ctx.change_tracker = (void *)&dummy_ct;
    HTTPRequest req = {0};

    handle_undo(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"undo\":true"));
    ck_assert(strstr(captured_body, "\"bytes_restored\":1024"));

    reset_stubs();
}

START_TEST(test_handle_redo_no_tracker)
{
    ServerContext ctx = {0};
    ctx.change_tracker = NULL;
    HTTPRequest req = {0};

    handle_redo(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 400);

    reset_stubs();
}

START_TEST(test_handle_redo_nothing)
{
    int dummy_ct = 0;
    stub_ct_redo_result = -1;

    ServerContext ctx = {0};
    ctx.change_tracker = (void *)&dummy_ct;
    HTTPRequest req = {0};

    handle_redo(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"redo\":false"));

    reset_stubs();
}

START_TEST(test_handle_redo_success)
{
    int dummy_ct = 0;
    stub_ct_redo_result = 2048;

    ServerContext ctx = {0};
    ctx.change_tracker = (void *)&dummy_ct;
    HTTPRequest req = {0};

    handle_redo(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"redo\":true"));
    ck_assert(strstr(captured_body, "\"bytes_written\":2048"));

    reset_stubs();
}

START_TEST(test_handle_health_detailed_no_sm)
{
    ServerContext ctx = {0};
    ctx.sm = NULL;
    ctx.state = STATE_LOCKED;
    HTTPRequest req = {0};

    handle_health_detailed(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"session_enabled\":false"));
    ck_assert(strstr(captured_body, "\"state\":\"locked\""));

    reset_stubs();
}

START_TEST(test_handle_health_detailed_with_sm)
{
    int dummy_sm = 0;
    ServerContext ctx = {0};
    ctx.sm = (void *)&dummy_sm;
    ctx.state = STATE_UNLOCKED;
    HTTPRequest req = {0};

    handle_health_detailed(&req, NULL, &ctx);
    ck_assert_int_eq(captured_status, 200);
    ck_assert(strstr(captured_body, "\"session_enabled\":true"));
    ck_assert(strstr(captured_body, "\"state\":\"unlocked\""));

    reset_stubs();
}


Suite *routes_general_general_suite(void)
{
    Suite *s = suite_create("routes_general_general");

    TCase *tc = tcase_create("handle_general");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_handle_health_ok);
    tcase_add_test(tc, test_handle_providers_lists_available_providers);
    tcase_add_test(tc, test_handle_status_locked);
    tcase_add_test(tc, test_handle_status_setup);
    tcase_add_test(tc, test_handle_status_unlocked_valid_token);
    tcase_add_test(tc, test_handle_status_unlocked_bad_token);
    tcase_add_test(tc, test_handle_status_noop_token_unlocked_without_header);
    tcase_add_test(tc, test_handle_status_session_enabled);
    tcase_add_test(tc, test_handle_config_no_agent);
    tcase_add_test(tc, test_handle_config_with_agent);
    tcase_add_test(tc, test_handle_metrics_no_metrics);
    tcase_add_test(tc, test_handle_metrics_success);
    tcase_add_test(tc, test_handle_metrics_render_null);
    tcase_add_test(tc, test_handle_undo_no_tracker);
    tcase_add_test(tc, test_handle_undo_nothing);
    tcase_add_test(tc, test_handle_undo_success);
    tcase_add_test(tc, test_handle_redo_no_tracker);
    tcase_add_test(tc, test_handle_redo_nothing);
    tcase_add_test(tc, test_handle_redo_success);
    tcase_add_test(tc, test_handle_health_detailed_no_sm);
    tcase_add_test(tc, test_handle_health_detailed_with_sm);

    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    Suite *s = routes_general_general_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_ENV);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed;
}
