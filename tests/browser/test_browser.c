/*
 * test_browser.c - unit tests for the browser subsystem: CDP transport
 * framing/id-matching/death handling against a fake pipe peer, and the
 * full navigate/evaluate/screenshot stack against a fake CDP peer over
 * a socketpair (no real browser process). Includes allocation-failure
 * regression tests per AGENTS.md.
 */

#define _GNU_SOURCE
#include <check.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/browser/cdp.h"
#include "../src/browser/browser.h"
#include "fake_cdp.h"

static const char *test_tmpdir(void)
{
    const char *tmp = getenv("TMPDIR");
    return (tmp && tmp[0]) ? tmp : "/tmp";
}

/* ---- helpers: build standard fake rule sets ---------------------- */

/* The three browser-level calls every page operation makes first. */
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

/* The extraction expression always contains "document.title". */
static void rules_add_extract(FakeCdpRule *rules, int *n, const char *inner)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "type", "string");
    cJSON_AddStringToObject(res, "value", inner);
    cJSON_AddItemToObject(payload, "result", res);
    rules[(*n)++] = (FakeCdpRule){.match = "document.title",
                                  .payload = payload};
}

static void rules_add_version(FakeCdpRule *rules, int *n)
{
    cJSON *ver = cJSON_CreateObject();
    cJSON_AddStringToObject(ver, "protocolVersion", "1.3");
    rules[(*n)++] = (FakeCdpRule){.match = "Browser.getVersion",
                                  .payload = ver};
}

static FakeCdpRule *rules_alloc(void)
{
    return calloc(16, sizeof(FakeCdpRule));
}

static void rules_free(FakeCdpRule *rules, int n)
{
    for (int i = 0; i < n; i++)
        if (rules[i].payload) cJSON_Delete(rules[i].payload);
    free(rules);
}

/* ---- CDP transport tests ----------------------------------------- */

START_TEST(test_cdp_roundtrip)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_version(rules, &n);
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);

    char *resp = cdp_client_call(c, "Browser.getVersion", NULL, NULL, 2000);
    ck_assert_ptr_nonnull(resp);
    cJSON *msg = cJSON_Parse(resp);
    ck_assert_ptr_nonnull(msg);
    cJSON *id = cJSON_GetObjectItem(msg, "id");
    ck_assert_int_eq(1, cJSON_IsNumber(id));
    ck_assert_int_eq(0, (int)id->valuedouble); /* first command id */
    cJSON *pv = cJSON_GetObjectItem(msg, "result");
    ck_assert_ptr_nonnull(pv);
    ck_assert_str_eq("1.3",
        cJSON_GetObjectItem(pv, "protocolVersion")->valuestring);
    cJSON_Delete(msg);
    free(resp);

    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_cdp_events_dropped)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules[n++] = (FakeCdpRule){
        .match = "Browser.getVersion",
        .prelude = "{\"method\":\"Page.loadEventFired\","
                   "\"params\":{\"timestamp\":1}}",
        .payload = NULL,
    };
    cJSON *ver = cJSON_CreateObject();
    cJSON_AddStringToObject(ver, "protocolVersion", "1.3");
    rules[n - 1].payload = ver;
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);

    char *resp = cdp_client_call(c, "Browser.getVersion", NULL, NULL, 2000);
    ck_assert_ptr_nonnull(resp);
    cJSON *msg = cJSON_Parse(resp);
    ck_assert_ptr_nonnull(msg);
    ck_assert_ptr_nonnull(cJSON_GetObjectItem(msg, "result"));
    cJSON_Delete(msg);
    free(resp);

    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_cdp_chunked_response)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    cJSON *ver = cJSON_CreateObject();
    cJSON_AddStringToObject(ver, "protocolVersion", "1.3");
    rules[n++] = (FakeCdpRule){.match = "Browser.getVersion",
                               .payload = ver, .chunks = 5};
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);

    char *resp = cdp_client_call(c, "Browser.getVersion", NULL, NULL, 3000);
    ck_assert_ptr_nonnull(resp);
    cJSON *msg = cJSON_Parse(resp);
    ck_assert_ptr_nonnull(msg);
    ck_assert_int_eq(0, cJSON_IsNumber(cJSON_GetObjectItem(msg, "id"))
                            ? 0 : -1);
    cJSON_Delete(msg);
    free(resp);

    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_cdp_timeout_leaves_transport_alive)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules[n++] = (FakeCdpRule){.match = "Slow.method", .drop = 1};
    cJSON *ver = cJSON_CreateObject();
    cJSON_AddStringToObject(ver, "protocolVersion", "1.3");
    rules[n++] = (FakeCdpRule){.match = "Browser.getVersion", .payload = ver};
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);

    char *resp = cdp_client_call(c, "Slow.method", NULL, NULL, 150);
    ck_assert_ptr_null(resp);
    ck_assert_int_eq(0, cdp_client_is_dead(c));

    resp = cdp_client_call(c, "Browser.getVersion", NULL, NULL, 2000);
    ck_assert_ptr_nonnull(resp);
    free(resp);

    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_cdp_eof_marks_dead)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_version(rules, &n);
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);

    char *resp = cdp_client_call(c, "Browser.getVersion", NULL, NULL, 2000);
    ck_assert_ptr_nonnull(resp);
    free(resp);

    fake_cdp_stop(fake); /* closes the peer end: reader sees EOF */

    /* Reader notices within a poll interval. */
    for (int i = 0; i < 50 && !cdp_client_is_dead(c); i++) usleep(20000);
    ck_assert_int_eq(1, cdp_client_is_dead(c));

    resp = cdp_client_call(c, "Browser.getVersion", NULL, NULL, 200);
    ck_assert_ptr_null(resp);

    cdp_client_close(c);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_cdp_malformed_line_dropped)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    cJSON *ver = cJSON_CreateObject();
    cJSON_AddStringToObject(ver, "protocolVersion", "1.3");
    rules[n++] = (FakeCdpRule){
        .match = "Browser.getVersion",
        .prelude = "this is not json",
        .payload = ver,
    };
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);

    char *resp = cdp_client_call(c, "Browser.getVersion", NULL, NULL, 2000);
    ck_assert_ptr_nonnull(resp);
    cJSON *msg = cJSON_Parse(resp);
    ck_assert_ptr_nonnull(msg);
    ck_assert_ptr_nonnull(cJSON_GetObjectItem(msg, "result"));
    cJSON_Delete(msg);
    free(resp);
    ck_assert_int_eq(0, cdp_client_is_dead(c));

    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_cdp_response_alloc_failure_marks_dead)
{
    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, NULL, 0);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);

    /* Direct dispatch reports OOM via -1; the reader thread is what
     * turns that into transport death (covered by the reader path in
     * other tests). */
    cdp_test_set_strdup_fail(1);
    ck_assert_int_eq(-1,
        cdp_test_handle_line(c,
            "{\"id\":7,\"result\":{\"ok\":true}}"));
    cdp_test_set_strdup_fail(-1);
    ck_assert_int_eq(0, cdp_client_is_dead(c));

    cdp_client_close(c);
    fake_cdp_stop(fake);
}
END_TEST

START_TEST(test_cdp_malformed_line_direct)
{
    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, NULL, 0);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);

    ck_assert_int_eq(0, cdp_test_handle_line(c, "not json at all"));
    ck_assert_int_eq(0, cdp_test_handle_line(c,
        "{\"method\":\"Page.frameNavigated\",\"params\":{}}"));
    ck_assert_int_eq(0, cdp_client_is_dead(c));

    cdp_client_close(c);
    fake_cdp_stop(fake);
}
END_TEST

/* ---- browser layer tests (attached sessions, fake peer) ---------- */

START_TEST(test_browser_navigate_happy_path)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_page(rules, &n);
    rules_add_navigate_ok(rules, &n);
    rules_add_ready(rules, &n, "complete");
    rules_add_extract(rules, &n,
        "{\"t\":\"Example\",\"u\":\"https://example.com/\","
        "\"x\":\"Hello world\"}");
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    BrowserPage page = {0};
    ck_assert_int_eq(0, browser_navigate(s, "https://example.com/",
                                         25000, 3000, &page));
    ck_assert_str_eq("Example", page.title);
    ck_assert_str_eq("https://example.com/", page.url);
    ck_assert_str_eq("Hello world", page.text);
    ck_assert_ptr_null(browser_last_error(s));
    browser_page_free(&page);

    /* Second navigate reuses the stored session id: no new target, so
     * exactly Page.navigate + readyState + extract are added. */
    int seen1 = fake->requests_seen;
    memset(&page, 0, sizeof(page));
    ck_assert_int_eq(0, browser_navigate(s, "https://example.com/2",
                                         25000, 3000, &page));
    ck_assert_int_eq(seen1 + 3, fake->requests_seen);
    browser_page_free(&page);

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_navigate_rejects_bad_scheme)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    BrowserPage page = {0};
    ck_assert_int_eq(-1,
        browser_navigate(s, "javascript:alert(1)", 1000, 1000, &page));
    ck_assert_ptr_null(page.title);
    ck_assert_ptr_null(page.url);
    ck_assert_ptr_null(page.text);
    const char *err = browser_last_error(s);
    ck_assert_ptr_nonnull(err);
    ck_assert_ptr_nonnull(strstr(err, "scheme"));

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_navigate_reports_navigation_error)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_page(rules, &n);
    cJSON *nav = cJSON_CreateObject();
    cJSON_AddStringToObject(nav, "frameId", "f1");
    cJSON_AddStringToObject(nav, "errorText",
                            "net::ERR_NAME_NOT_RESOLVED");
    rules[n++] = (FakeCdpRule){.match = "Page.navigate", .payload = nav};
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    BrowserPage page = {0};
    ck_assert_int_eq(-1,
        browser_navigate(s, "https://no-such-host.invalid/", 1000,
                         2000, &page));
    ck_assert_ptr_nonnull(strstr(browser_last_error(s),
                                 "ERR_NAME_NOT_RESOLVED"));

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_navigate_still_loading_note)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_page(rules, &n);
    rules_add_navigate_ok(rules, &n);
    rules_add_ready(rules, &n, "loading");
    rules_add_extract(rules, &n,
        "{\"t\":\"SPA\",\"u\":\"https://app.example/\",\"x\":\"partial\"}");
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    BrowserPage page = {0};
    ck_assert_int_eq(0, browser_navigate(s, "https://app.example/",
                                         1000, 600, &page));
    ck_assert_str_eq("SPA", page.title);
    ck_assert_ptr_nonnull(strstr(page.text, "still loading"));
    browser_page_free(&page);

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_navigate_truncates_text)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_page(rules, &n);
    rules_add_navigate_ok(rules, &n);
    rules_add_ready(rules, &n, "complete");
    char big[512];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    char inner[600];
    snprintf(inner, sizeof(inner),
             "{\"t\":\"Big\",\"u\":\"https://big.example/\",\"x\":\"%s\"}",
             big);
    rules_add_extract(rules, &n, inner);
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    BrowserPage page = {0};
    ck_assert_int_eq(0, browser_navigate(s, "https://big.example/",
                                         100, 2000, &page));
    ck_assert_ptr_nonnull(strstr(page.text, "[... truncated"));
    browser_page_free(&page);

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_evaluate_returns_json)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_page(rules, &n);
    cJSON *payload = cJSON_CreateObject();
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "type", "object");
    cJSON *val = cJSON_CreateObject();
    cJSON_AddNumberToObject(val, "a", 1);
    cJSON_AddItemToObject(res, "value", val);
    cJSON_AddItemToObject(payload, "result", res);
    rules[n++] = (FakeCdpRule){.match = "Runtime.evaluate",
                               .payload = payload};
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    char *json = NULL;
    ck_assert_int_eq(0, browser_evaluate(s, "({a:1})", &json, 2000));
    ck_assert_ptr_nonnull(json);
    cJSON *parsed = cJSON_Parse(json);
    ck_assert_ptr_nonnull(parsed);
    ck_assert_int_eq(1, cJSON_GetObjectItem(parsed, "a")->valueint);
    cJSON_Delete(parsed);
    free(json);

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_evaluate_reports_exception)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_page(rules, &n);
    cJSON *payload = cJSON_CreateObject();
    cJSON *exc = cJSON_CreateObject();
    cJSON_AddStringToObject(exc, "text", "TypeError");
    cJSON *excobj = cJSON_CreateObject();
    cJSON_AddStringToObject(excobj, "description",
                            "TypeError: x is not a function");
    cJSON_AddItemToObject(exc, "exception", excobj);
    cJSON_AddItemToObject(payload, "exceptionDetails", exc);
    rules[n++] = (FakeCdpRule){.match = "Runtime.evaluate",
                               .payload = payload};
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    char *json = NULL;
    ck_assert_int_eq(-1, browser_evaluate(s, "nope()", &json, 2000));
    ck_assert_ptr_null(json);
    ck_assert_ptr_nonnull(strstr(browser_last_error(s), "TypeError"));

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_screenshot_writes_png)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_page(rules, &n);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "data", "iVBORw0KGgo=");
    rules[n++] = (FakeCdpRule){.match = "Page.captureScreenshot",
                               .payload = payload};
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    char path[512];
    snprintf(path, sizeof(path), "%s/echo_ai_shot_%ld.png",
             test_tmpdir(), (long)getpid());
    ck_assert_int_eq(0, browser_screenshot(s, path, 2000));

    FILE *f = fopen(path, "rb");
    ck_assert_ptr_nonnull(f);
    unsigned char magic[8];
    ck_assert_int_eq(8, (int)fread(magic, 1, 8, f));
    fclose(f);
    static const unsigned char png_magic[8] =
        {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    ck_assert_int_eq(0, memcmp(magic, png_magic, 8));
    unlink(path);

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_screenshot_bad_base64)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_page(rules, &n);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "data", "!!!not-base64!!!");
    rules[n++] = (FakeCdpRule){.match = "Page.captureScreenshot",
                               .payload = payload};
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    ck_assert_int_eq(-1, browser_screenshot(s, "/tmp/should-not-exist",
                                            2000));
    ck_assert_ptr_nonnull(browser_last_error(s));

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_click_sends_press_and_release)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_page(rules, &n);
    /* Two Input.dispatchMouseEvent calls: mousePressed then
     * mouseReleased. Both match the same rule and return an empty
     * result. */
    rules[n++] = (FakeCdpRule){.match = "Input.dispatchMouseEvent",
                               .payload = cJSON_CreateObject()};
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    /* First click: page setup (createTarget + attachToTarget) plus the
     * two mouse events = 4 requests. */
    ck_assert_int_eq(0, browser_click(s, 123, 456, NULL, 2000));
    ck_assert_int_eq(4, fake->requests_seen);

    /* Second click reuses the session id: only the two mouse events. */
    int seen = fake->requests_seen;
    ck_assert_int_eq(0, browser_click(s, 7, 8, NULL, 2000));
    ck_assert_int_eq(seen + 2, fake->requests_seen);

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_click_button_is_string_enum)
{
    /* Regression: CDP Input.dispatchMouseEvent declares `button` as the
     * MouseButton string enum ("left"); sending a JSON number makes real
     * Chromium reject the call with "Invalid parameters", which surfaced
     * as the click tool failing with Input.dispatchMouseEvent: Invalid
     * parameters. */
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_page(rules, &n);
    rules[n++] = (FakeCdpRule){.match = "Input.dispatchMouseEvent",
                               .payload = cJSON_CreateObject()};
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    /* NULL button defaults to "left" (the primary path). */
    ck_assert_int_eq(0, browser_click(s, 457, 389, NULL, 2000));

    /* The last request is the mouseReleased half; both halves carry the
     * same params, so inspecting this one covers them. */
    cJSON *req = fake_cdp_last_request_copy(fake);
    ck_assert_ptr_nonnull(req);
    cJSON *params = cJSON_GetObjectItem(req, "params");
    ck_assert_ptr_nonnull(params);
    cJSON *btn = cJSON_GetObjectItem(params, "button");
    ck_assert_ptr_nonnull(btn);
    ck_assert_int_eq(1, cJSON_IsString(btn));
    ck_assert_str_eq("left", btn->valuestring);
    cJSON *x = cJSON_GetObjectItem(params, "x");
    cJSON *y = cJSON_GetObjectItem(params, "y");
    ck_assert_ptr_nonnull(x);
    ck_assert_ptr_nonnull(y);
    ck_assert_int_eq(457, (int)x->valuedouble);
    ck_assert_int_eq(389, (int)y->valuedouble);
    cJSON_Delete(req);

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_click_uses_chosen_button)
{
    /* A non-default button must pass through to both halves of the
     * click, still as the string enum. */
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_page(rules, &n);
    rules[n++] = (FakeCdpRule){.match = "Input.dispatchMouseEvent",
                               .payload = cJSON_CreateObject()};
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    ck_assert_int_eq(0, browser_click(s, 10, 20, "right", 2000));
    ck_assert_int_eq(4, fake->requests_seen);

    cJSON *req = fake_cdp_last_request_copy(fake);
    ck_assert_ptr_nonnull(req);
    cJSON *params = cJSON_GetObjectItem(req, "params");
    ck_assert_ptr_nonnull(params);
    cJSON *btn = cJSON_GetObjectItem(params, "button");
    ck_assert_ptr_nonnull(btn);
    ck_assert_int_eq(1, cJSON_IsString(btn));
    ck_assert_str_eq("right", btn->valuestring);
    cJSON_Delete(req);

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_click_rejects_bad_button)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_page(rules, &n);
    rules[n++] = (FakeCdpRule){.match = "Input.dispatchMouseEvent",
                               .payload = cJSON_CreateObject()};
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    /* Validation happens before any page setup, so nothing goes on the
     * wire: requests_seen stays 0. */
    ck_assert_int_eq(-1, browser_click(s, 10, 20, "sideways", 2000));
    ck_assert_int_eq(0, fake->requests_seen);
    ck_assert_ptr_nonnull(browser_last_error(s));
    ck_assert_ptr_nonnull(strstr(browser_last_error(s), "unsupported"));

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_fetch_text)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_page(rules, &n);
    rules_add_navigate_ok(rules, &n);
    rules_add_ready(rules, &n, "complete");
    rules_add_extract(rules, &n,
        "{\"t\":\"Doc\",\"u\":\"https://docs.example/\","
        "\"x\":\"body text\"}");
    rules[n++] = (FakeCdpRule){.match = NULL};

    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);

    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    char *text = browser_fetch_text(s, "https://docs.example/",
                                    25000, 3000);
    ck_assert_ptr_nonnull(text);
    ck_assert_ptr_nonnull(strstr(text, "Title: Doc"));
    ck_assert_ptr_nonnull(strstr(text, "body text"));
    free(text);

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_base64_vectors)
{
    static const struct {
        const char *in;
        const char *expect;
    } vectors[] = {
        {"aGVsbG8=", "hello"},
        {"iVBORw0KGgo=", "\x89PNG\r\n\x1a\n"},
        {" TWFu\n", "Man"},
        {"", NULL},
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++)
    {
        size_t len = 0;
        unsigned char *out = browser_test_decode_base64(vectors[i].in,
                                                        &len);
        if (!vectors[i].expect)
        {
            ck_assert_ptr_null(out);
            continue;
        }
        ck_assert_ptr_nonnull(out);
        ck_assert_int_eq((int)strlen(vectors[i].expect), (int)len);
        ck_assert_int_eq(0, memcmp(out, vectors[i].expect, len));
        free(out);
    }

    size_t len = 0;
    ck_assert_ptr_null(browser_test_decode_base64("!!!!", &len));
    ck_assert_ptr_null(browser_test_decode_base64("abc", &len));
}
END_TEST

START_TEST(test_browser_navigate_alloc_failures)
{
    FakeCdpRule *rules = rules_alloc();
    int n = 0;
    rules_add_page(rules, &n);
    rules_add_navigate_ok(rules, &n);
    rules_add_ready(rules, &n, "complete");
    rules_add_extract(rules, &n,
        "{\"t\":\"T\",\"u\":\"https://x.example/\",\"x\":\"hi\"}");
    rules[n++] = (FakeCdpRule){.match = NULL};

    /* Fault-inject every strdup index the happy path can hit: each
     * must fail cleanly (no crash, no partial page, error reported)
     * or leave the path untouched; ASan proves nothing leaked. */
    for (int k = 1; k <= 10; k++)
    {
        int client_fd = -1;
        FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
        ck_assert_ptr_nonnull(fake);

        CdpClient *c = cdp_client_new(client_fd, client_fd);
        ck_assert_ptr_nonnull(c);
        BrowserSession *s = browser_test_attach(c);
        ck_assert_ptr_nonnull(s);

        browser_test_set_strdup_fail(k);
        BrowserPage page = {0};
        int rc = browser_navigate(s, "https://x.example/", 1000,
                                  2000, &page);
        browser_test_set_strdup_fail(-1);
        if (rc != 0)
        {
            ck_assert_ptr_null(page.title);
            ck_assert_ptr_null(page.url);
            ck_assert_ptr_null(page.text);
            ck_assert_ptr_nonnull(browser_last_error(s));
        }
        browser_page_free(&page);

        browser_close(s);
        cdp_client_close(c);
        fake_cdp_stop(fake);
    }

    /* After the storm, the clean path still works end to end. */
    int client_fd = -1;
    FakeCdp *fake = fake_cdp_start(&client_fd, rules, n);
    ck_assert_ptr_nonnull(fake);
    CdpClient *c = cdp_client_new(client_fd, client_fd);
    ck_assert_ptr_nonnull(c);
    BrowserSession *s = browser_test_attach(c);
    ck_assert_ptr_nonnull(s);

    BrowserPage page = {0};
    ck_assert_int_eq(0, browser_navigate(s, "https://x.example/",
                                         1000, 2000, &page));
    ck_assert_str_eq("T", page.title);
    browser_page_free(&page);

    browser_close(s);
    cdp_client_close(c);
    fake_cdp_stop(fake);
    rules_free(rules, n);
}
END_TEST

START_TEST(test_browser_open_binary_not_found)
{
    /* Discovery must fail cleanly when nothing is configured.
     * ECHO_BROWSER_NO_FALLBACK skips the PATH-name/macOS-bundle lists,
     * which would otherwise find a real browser on machines that have
     * one installed (e.g. macOS CI runners). */
    char *old_bin = getenv("ECHO_BROWSER_BIN");
    char *old_path = getenv("PATH");
    char *old_browser = getenv("BROWSER");
    char *old_nofb = getenv("ECHO_BROWSER_NO_FALLBACK");
    setenv("ECHO_BROWSER_BIN", "", 1);
    unsetenv("BROWSER");
    setenv("PATH", "/nonexistent-dir", 1);
    setenv("ECHO_BROWSER_NO_FALLBACK", "1", 1);

    char *err = NULL;
    BrowserSession *s = browser_open(NULL, &err);
    ck_assert_ptr_null(s);
    ck_assert_ptr_nonnull(err);
    ck_assert_ptr_nonnull(strstr(err, "ECHO_BROWSER_BIN"));
    free(err);

    if (old_bin) setenv("ECHO_BROWSER_BIN", old_bin, 1);
    else unsetenv("ECHO_BROWSER_BIN");
    if (old_path) setenv("PATH", old_path, 1);
    else unsetenv("PATH");
    if (old_browser) setenv("BROWSER", old_browser, 1);
    else unsetenv("BROWSER");
    if (old_nofb) setenv("ECHO_BROWSER_NO_FALLBACK", old_nofb, 1);
    else unsetenv("ECHO_BROWSER_NO_FALLBACK");
}
END_TEST

START_TEST(test_browser_open_binary_exits_immediately)
{
    /* Point at a non-executable file: exec fails, the child exits 127,
     * and browser_open must report that instead of hanging. */
    char path[512];
    snprintf(path, sizeof(path), "%s/echo_ai_fake_browser_%ld",
             test_tmpdir(), (long)getpid());
    FILE *f = fopen(path, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "#!/bin/sh\nexit 0\n");
    fclose(f);
    chmod(path, 0700);

    char *old_bin = getenv("ECHO_BROWSER_BIN");
    char *old_headless = getenv("ECHO_BROWSER_HEADLESS");
    char *old_profile = getenv("ECHO_BROWSER_USER_DATA_DIR");
    setenv("ECHO_BROWSER_BIN", path, 1);
    unsetenv("ECHO_BROWSER_HEADLESS");
    setenv("ECHO_BROWSER_USER_DATA_DIR",
           "/tmp/echo_ai_profile_fake", 1);

    char *err = NULL;
    BrowserSession *s = browser_open(NULL, &err);
    ck_assert_ptr_null(s);
    ck_assert_ptr_nonnull(err);

    if (old_bin) setenv("ECHO_BROWSER_BIN", old_bin, 1);
    else unsetenv("ECHO_BROWSER_BIN");
    if (old_headless) setenv("ECHO_BROWSER_HEADLESS", old_headless, 1);
    else unsetenv("ECHO_BROWSER_HEADLESS");
    if (old_profile) setenv("ECHO_BROWSER_USER_DATA_DIR", old_profile, 1);
    else unsetenv("ECHO_BROWSER_USER_DATA_DIR");
    unlink(path);
    free(err);
}
END_TEST

/* ---- suite ------------------------------------------------------- */

START_TEST(test_browser_profile_removal_races_writers)
{
    /* Regression for the "Directory not empty" close race: a straggler
     * process (orphaned renderer in real use) keeps creating profile
     * files while rm runs. Removal must retry until the dir is gone. */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/echo_ai_profile_test_%ld",
             test_tmpdir(), (long)getpid());
    ck_assert_int_eq(0, mkdir(dir, 0700));

    char first[600];
    snprintf(first, sizeof(first), "%s/seed", dir);
    FILE *f = fopen(first, "w");
    ck_assert_ptr_nonnull(f);
    fclose(f);

    /* Straggler: keeps dropping files into the dir for ~500 ms. */
    pid_t writer = fork();
    ck_assert_int_ge(writer, 0);
    if (writer == 0)
    {
        char path[600];
        for (int i = 0; i < 10; i++)
        {
            snprintf(path, sizeof(path), "%s/writer_%d", dir, i);
            int fd = open(path, O_WRONLY | O_CREAT, 0600);
            if (fd >= 0) close(fd);
            usleep(50000);
        }
        _exit(0);
    }

    browser_test_remove_profile_dir(dir);

    ck_assert_int_eq(-1, access(dir, F_OK));
    int status = 0;
    waitpid(writer, &status, 0);
}
END_TEST

START_TEST(test_browser_profile_removal_simple)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/echo_ai_profile_plain_%ld",
             test_tmpdir(), (long)getpid());
    ck_assert_int_eq(0, mkdir(dir, 0700));
    char sub[600];
    snprintf(sub, sizeof(sub), "%s/nested", dir);
    ck_assert_int_eq(0, mkdir(sub, 0700));
    char file[600];
    snprintf(file, sizeof(file), "%s/nested/leaf", dir);
    FILE *f = fopen(file, "w");
    ck_assert_ptr_nonnull(f);
    fclose(f);

    browser_test_remove_profile_dir(dir);
    ck_assert_int_eq(-1, access(dir, F_OK));
}
END_TEST

static Suite *browser_suite(void)
{
    Suite *s = suite_create("browser");

    TCase *tc_cdp = tcase_create("cdp_transport");
    tcase_set_timeout(tc_cdp, 15);
    tcase_add_test(tc_cdp, test_cdp_roundtrip);
    tcase_add_test(tc_cdp, test_cdp_events_dropped);
    tcase_add_test(tc_cdp, test_cdp_chunked_response);
    tcase_add_test(tc_cdp, test_cdp_timeout_leaves_transport_alive);
    tcase_add_test(tc_cdp, test_cdp_eof_marks_dead);
    tcase_add_test(tc_cdp, test_cdp_malformed_line_dropped);
    tcase_add_test(tc_cdp, test_cdp_response_alloc_failure_marks_dead);
    tcase_add_test(tc_cdp, test_cdp_malformed_line_direct);
    suite_add_tcase(s, tc_cdp);

    TCase *tc_browser = tcase_create("browser_layer");
    tcase_set_timeout(tc_browser, 30);
    tcase_add_test(tc_browser, test_browser_navigate_happy_path);
    tcase_add_test(tc_browser, test_browser_navigate_rejects_bad_scheme);
    tcase_add_test(tc_browser, test_browser_navigate_reports_navigation_error);
    tcase_add_test(tc_browser, test_browser_navigate_still_loading_note);
    tcase_add_test(tc_browser, test_browser_navigate_truncates_text);
    tcase_add_test(tc_browser, test_browser_evaluate_returns_json);
    tcase_add_test(tc_browser, test_browser_evaluate_reports_exception);
    tcase_add_test(tc_browser, test_browser_screenshot_writes_png);
    tcase_add_test(tc_browser, test_browser_screenshot_bad_base64);
    tcase_add_test(tc_browser, test_browser_click_sends_press_and_release);
    tcase_add_test(tc_browser, test_browser_click_button_is_string_enum);
    tcase_add_test(tc_browser, test_browser_click_uses_chosen_button);
    tcase_add_test(tc_browser, test_browser_click_rejects_bad_button);
    tcase_add_test(tc_browser, test_browser_fetch_text);
    tcase_add_test(tc_browser, test_browser_base64_vectors);
    tcase_add_test(tc_browser, test_browser_navigate_alloc_failures);
    tcase_add_test(tc_browser, test_browser_open_binary_not_found);
    tcase_add_test(tc_browser, test_browser_open_binary_exits_immediately);
    tcase_add_test(tc_browser, test_browser_profile_removal_races_writers);
    tcase_add_test(tc_browser, test_browser_profile_removal_simple);
    suite_add_tcase(s, tc_browser);

    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(browser_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? 0 : 1;
}
