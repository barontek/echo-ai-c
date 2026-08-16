/*
 * test_tool_browser.c - unit tests for the browser tool: action
 * dispatch, argument validation, auto-open, and lifecycle, driven
 * against an injected session backed by the fake CDP peer (no real
 * browser process).
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../src/browser/cdp.h"
#include "../../src/browser/browser.h"
#include "../../src/safety/safety.h"
#include "../../src/tools/tool.h"
#include "fake_cdp.h"

/* Declared in tool_browser.c under TOOL_BROWSER_TEST. */
extern Tool *tool_browser_create(SafetyConfig *safety);
extern void tool_browser_test_set_session(Tool *self, BrowserSession *s);
extern void tool_browser_set_headless(Tool *self, int headless);

static const char *test_tmpdir(void)
{
    const char *tmp = getenv("TMPDIR");
    return (tmp && tmp[0]) ? tmp : "/tmp";
}

static void rules_add_page(FakeCdpRule *rules, int *n)
{
    cJSON *tid = cJSON_CreateObject();
    cJSON_AddStringToObject(tid, "targetId", "t1");
    rules[(*n)++] = (FakeCdpRule){.match = "Target.createTarget",
                                  .payload = tid};
    cJSON *sid = cJSON_CreateObject();
    cJSON_AddStringToObject(sid, "sessionId", "s1");
    rules[(*n)++] = (FakeCdpRule){.match = "Target.attachToTarget",
                                  .payload = sid};
}

static void rules_add_navigate_ok(FakeCdpRule *rules, int *n)
{
    cJSON *nav = cJSON_CreateObject();
    cJSON_AddStringToObject(nav, "frameId", "f1");
    rules[(*n)++] = (FakeCdpRule){.match = "Page.navigate",
                                  .payload = nav};
}

static void rules_add_ready(FakeCdpRule *rules, int *n, const char *state)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "type", "string");
    /* Real CDP: the string value is bare; cJSON quotes it on the wire. */
    cJSON_AddStringToObject(res, "value", state);
    cJSON_AddItemToObject(payload, "result", res);
    rules[(*n)++] = (FakeCdpRule){.match = "document.readyState",
                                  .payload = payload};
}

static void rules_add_extract(FakeCdpRule *rules, int *n)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "type", "string");
    cJSON_AddStringToObject(res, "value",
        "{\"t\":\"ToolPage\",\"u\":\"https://tool.example/\","
        "\"x\":\"tool body\"}");
    cJSON_AddItemToObject(payload, "result", res);
    rules[(*n)++] = (FakeCdpRule){.match = "document.title",
                                  .payload = payload};
}

static void rules_add_screenshot(FakeCdpRule *rules, int *n)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "data", "iVBORw0KGgo=");
    rules[(*n)++] = (FakeCdpRule){.match = "Page.captureScreenshot",
                                  .payload = payload};
}

static void rules_add_generic_evaluate(FakeCdpRule *rules, int *n)
{
    /* Catch-all for anything else (evaluate "1+1" etc.): a valid
     * Runtime.evaluate-shaped payload. */
    cJSON *payload = cJSON_CreateObject();
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "type", "string");
    cJSON_AddStringToObject(res, "value", "42");
    cJSON_AddItemToObject(payload, "result", res);
    rules[(*n)++] = (FakeCdpRule){.match = NULL, .payload = payload};
}

typedef struct {
    FakeCdp *fake;
    CdpClient *client;
    Tool *tool;
    FakeCdpRule *rules; /* borrowed by the fake thread until stop */
    int rule_count;
} Fixture;

/* Session backed by the fake peer, injected into a fresh tool. */
static Fixture fixture_new(void)
{
    Fixture fx = {0};
    fx.rules = calloc(16, sizeof(FakeCdpRule));
    ck_assert_ptr_nonnull(fx.rules);
    int n = 0;
    rules_add_page(fx.rules, &n);
    rules_add_navigate_ok(fx.rules, &n);
    rules_add_ready(fx.rules, &n, "complete");
    rules_add_extract(fx.rules, &n);
    rules_add_screenshot(fx.rules, &n);
    rules_add_generic_evaluate(fx.rules, &n);

    int client_fd = -1;
    fx.fake = fake_cdp_start(&client_fd, fx.rules, n);
    ck_assert_ptr_nonnull(fx.fake);
    fx.rule_count = n;
    fx.client = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(fx.client);
    BrowserSession *s = browser_test_attach(fx.client);
    ck_assert_ptr_nonnull(s);

    fx.tool = tool_browser_create(NULL);
    ck_assert_ptr_nonnull(fx.tool);
    tool_browser_test_set_session(fx.tool, s);
    return fx;
}

static void fixture_free(Fixture *fx)
{
    fx->tool->destroy(fx->tool);
    cdp_client_close(fx->client);
    fake_cdp_stop(fx->fake); /* joins the thread: rules are free now */
    for (int i = 0; i < fx->rule_count; i++)
        if (fx->rules[i].payload) cJSON_Delete(fx->rules[i].payload);
    free(fx->rules);
    memset(fx, 0, sizeof(*fx));
}

static ToolResult *run(Tool *t, const char *json)
{
    return t->execute(t, json);
}

START_TEST(test_tool_browser_navigate)
{
    Fixture fx = fixture_new();
    ToolResult *r = run(fx.tool,
        "{\"action\":\"navigate\",\"url\":\"https://tool.example/\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_nonnull(r->content);
    ck_assert_ptr_nonnull(strstr(r->content, "Title: ToolPage"));
    ck_assert_ptr_nonnull(strstr(r->content, "tool body"));
    tool_result_free(r);
    fixture_free(&fx);
}
END_TEST

START_TEST(test_tool_browser_navigate_defaults)
{
    /* max_chars/timeout omitted: defaults apply. */
    Fixture fx = fixture_new();
    ToolResult *r = run(fx.tool,
        "{\"action\":\"navigate\",\"url\":\"https://tool.example/\"}");
    ck_assert_ptr_null(r->error);
    tool_result_free(r);
    fixture_free(&fx);
}
END_TEST

START_TEST(test_tool_browser_evaluate)
{
    Fixture fx = fixture_new();
    ToolResult *r = run(fx.tool,
        "{\"action\":\"evaluate\",\"expression\":\"1+1\"}");
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_nonnull(r->content);
    /* The result is the JSON-encoded value, as a real browser sends it. */
    ck_assert_str_eq("\"42\"", r->content);
    tool_result_free(r);
    fixture_free(&fx);
}
END_TEST

START_TEST(test_tool_browser_screenshot)
{
    Fixture fx = fixture_new();
    char path[512];
    snprintf(path, sizeof(path), "%s/echo_ai_tool_shot_%ld.png",
             test_tmpdir(), (long)getpid());
    char args[600];
    snprintf(args, sizeof(args),
             "{\"action\":\"screenshot\",\"path\":\"%s\"}", path);
    ToolResult *r = run(fx.tool, args);
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_nonnull(r->content);
    ck_assert_ptr_nonnull(strstr(r->content, path));
    tool_result_free(r);

    FILE *f = fopen(path, "rb");
    ck_assert_ptr_nonnull(f);
    unsigned char magic[8];
    ck_assert_int_eq(8, (int)fread(magic, 1, 8, f));
    fclose(f);
    static const unsigned char png_magic[8] =
        {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    ck_assert_int_eq(0, memcmp(magic, png_magic, 8));
    unlink(path);

    fixture_free(&fx);
}
END_TEST

START_TEST(test_tool_browser_validation_errors)
{
    Fixture fx = fixture_new();

    ToolResult *r = run(fx.tool, "{\"action\":\"navigate\"}");
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq("validation_error", r->error_category);
    tool_result_free(r);

    r = run(fx.tool, "{\"action\":\"evaluate\"}");
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq("validation_error", r->error_category);
    tool_result_free(r);

    r = run(fx.tool, "{\"action\":\"teleport\"}");
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq("validation_error", r->error_category);
    ck_assert_ptr_nonnull(strstr(r->error, "unknown action"));
    tool_result_free(r);

    r = run(fx.tool, "{\"not\":\"an action\"}");
    ck_assert_ptr_nonnull(r->error);
    tool_result_free(r);

    r = run(fx.tool, "not json");
    ck_assert_ptr_nonnull(r->error);
    tool_result_free(r);

    fixture_free(&fx);
}
END_TEST

START_TEST(test_tool_browser_open_already_open)
{
    Fixture fx = fixture_new();
    ToolResult *r = run(fx.tool, "{\"action\":\"open\"}");
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_nonnull(strstr(r->content, "already open"));
    tool_result_free(r);
    fixture_free(&fx);
}
END_TEST

START_TEST(test_tool_browser_status_and_close)
{
    Fixture fx = fixture_new();
    ToolResult *r = run(fx.tool, "{\"action\":\"status\"}");
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_nonnull(strstr(r->content, "running"));
    tool_result_free(r);

    r = run(fx.tool, "{\"action\":\"close\"}");
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_nonnull(strstr(r->content, "closed"));
    tool_result_free(r);

    r = run(fx.tool, "{\"action\":\"status\"}");
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_nonnull(strstr(r->content, "not running"));
    tool_result_free(r);

    r = run(fx.tool, "{\"action\":\"close\"}");
    ck_assert_ptr_null(r->error);
    tool_result_free(r);

    fixture_free(&fx);
}
END_TEST

START_TEST(test_tool_browser_open_failure_reports_error)
{
    /* No injected session: navigate auto-opens and must fail with a
     * useful message when no browser is discoverable. */
    char *old_bin = getenv("ECHO_BROWSER_BIN");
    char *old_path = getenv("PATH");
    char *old_browser = getenv("BROWSER");
    setenv("ECHO_BROWSER_BIN", "", 1);
    unsetenv("BROWSER");
    setenv("PATH", "/nonexistent-dir", 1);

    Tool *t = tool_browser_create(NULL);
    ck_assert_ptr_nonnull(t);
    ToolResult *r = run(t,
        "{\"action\":\"navigate\",\"url\":\"https://tool.example/\"}");
    ck_assert_ptr_nonnull(r->error);
    ck_assert_ptr_nonnull(strstr(r->error, "ECHO_BROWSER_BIN"));
    tool_result_free(r);
    t->destroy(t);

    if (old_bin) setenv("ECHO_BROWSER_BIN", old_bin, 1);
    else unsetenv("ECHO_BROWSER_BIN");
    if (old_path) setenv("PATH", old_path, 1);
    else unsetenv("PATH");
    if (old_browser) setenv("BROWSER", old_browser, 1);
    else unsetenv("BROWSER");
}
END_TEST

START_TEST(test_tool_browser_set_headless)
{
    /* The mode only affects spawn, so with an injected session every
     * action must behave identically in both modes. */
    Fixture fx = fixture_new();
    tool_browser_set_headless(fx.tool, 1);
    ToolResult *r = run(fx.tool,
        "{\"action\":\"navigate\",\"url\":\"https://tool.example/\"}");
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_nonnull(strstr(r->content, "Title: ToolPage"));
    tool_result_free(r);

    tool_browser_set_headless(fx.tool, 0);
    r = run(fx.tool, "{\"action\":\"evaluate\",\"expression\":\"1\"}");
    ck_assert_ptr_null(r->error);
    tool_result_free(r);

    fixture_free(&fx);
}
END_TEST

static Suite *tool_browser_suite(void)
{
    Suite *s = suite_create("tool_browser");

    TCase *tc = tcase_create("tool_browser_actions");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_tool_browser_navigate);
    tcase_add_test(tc, test_tool_browser_navigate_defaults);
    tcase_add_test(tc, test_tool_browser_evaluate);
    tcase_add_test(tc, test_tool_browser_screenshot);
    tcase_add_test(tc, test_tool_browser_validation_errors);
    tcase_add_test(tc, test_tool_browser_open_already_open);
    tcase_add_test(tc, test_tool_browser_status_and_close);
    tcase_add_test(tc, test_tool_browser_open_failure_reports_error);
    tcase_add_test(tc, test_tool_browser_set_headless);
    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(tool_browser_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? 0 : 1;
}
