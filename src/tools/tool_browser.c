/*
 * tool_browser.c - Visible browser tool: opens a Chromium-family
 * browser window on the user's desktop (headful by default) and lets
 * the LLM navigate pages, read page text, run JavaScript, and take
 * screenshots. The browser binary is never hardcoded: ECHO_BROWSER_BIN,
 * $BROWSER, PATH lookup, or a per-call "binary" argument. Depends on:
 * tool.h, browser.h, string_utils.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tool.h"
#include "../browser/browser.h"
#include "../safety/safety.h"
#include "../utils/string_utils.h"

typedef struct {
    BrowserSession *session;
    int headless; /* 0 = visible window (default); 1 = no window */
} BrowserCtx;

static ToolResult *tool_browser_open(Tool *self, cJSON *args)
{
    BrowserCtx *ctx = self->ctx;
    if (ctx->session)
        return tool_result_create("browser window is already open");

    cJSON *bin = cJSON_GetObjectItem(args, "binary");
    const char *binary = cJSON_IsString(bin) ? bin->valuestring : NULL;

    BrowserConfig cfg = {0};
    cfg.binary = binary;
    cfg.headless = ctx->headless;
    char *err = NULL;
    ctx->session = browser_open(&cfg, &err);
    if (!ctx->session)
    {
        ToolResult *r = tool_result_error(err ? err : "failed to open browser",
                                          "execution_error");
        free(err);
        return r;
    }
    return tool_result_create("browser window opened on the desktop; "
                              "the user can watch and interact with it");
}

/* All actions that touch a page auto-open the browser first. */
static ToolResult *tool_browser_ensure(Tool *self, cJSON *args)
{
    BrowserCtx *ctx = self->ctx;
    if (ctx->session) return NULL;
    ToolResult *r = tool_browser_open(self, args);
    return r;
}

static int tool_browser_get_timeout(cJSON *args)
{
    cJSON *t = cJSON_GetObjectItem(args, "timeout");
    if (!cJSON_IsNumber(t)) return 30000;
    double secs = t->valuedouble;
    if (secs <= 0) secs = 30;
    int ms = (int)(secs * 1000);
    return ms > 0 ? ms : 30000;
}

static size_t tool_browser_get_max_chars(cJSON *args)
{
    cJSON *m = cJSON_GetObjectItem(args, "max_chars");
    if (!cJSON_IsNumber(m)) return 25000;
    double v = m->valuedouble;
    if (v <= 0) return 25000;
    return (size_t)v;
}

static ToolResult *tool_browser_navigate(Tool *self, cJSON *args)
{
    cJSON *url_j = cJSON_GetObjectItem(args, "url");
    if (!cJSON_IsString(url_j))
        return tool_result_error("navigate requires a 'url' string",
                                 "validation_error");

    ToolResult *open_result = tool_browser_ensure(self, args);
    if (open_result) return open_result;

    BrowserCtx *ctx = self->ctx;
    int ms = tool_browser_get_timeout(args);
    size_t cap = tool_browser_get_max_chars(args);

    BrowserPage page = {0};
    if (browser_navigate(ctx->session, url_j->valuestring, cap, ms,
                         &page) != 0)
    {
        const char *err = browser_last_error(ctx->session);
        return tool_result_error(err ? err : "navigation failed",
                                 "execution_error");
    }

    char head[4200];
    int n = snprintf(head, sizeof(head), "Title: %s\nURL: %s\n\n",
                     page.title ? page.title : "",
                     page.url ? page.url : "");
    if (n <= 0 || (size_t)n >= sizeof(head))
    {
        browser_page_free(&page);
        return tool_result_error("page metadata too long",
                                 "execution_error");
    }
    char *content = str_dup(head);
    if (content && page.text) str_append(&content, page.text);
    browser_page_free(&page);
    if (!content)
        return tool_result_error("out of memory", "execution_error");
    ToolResult *tr = tool_result_create(content);
    free(content);
    return tr;
}

static ToolResult *tool_browser_evaluate(Tool *self, cJSON *args)
{
    cJSON *expr = cJSON_GetObjectItem(args, "expression");
    if (!cJSON_IsString(expr))
        return tool_result_error("evaluate requires an 'expression' string",
                                 "validation_error");

    ToolResult *open_result = tool_browser_ensure(self, args);
    if (open_result) return open_result;

    BrowserCtx *ctx = self->ctx;
    int ms = tool_browser_get_timeout(args);
    char *json = NULL;
    if (browser_evaluate(ctx->session, expr->valuestring, &json, ms) != 0)
    {
        const char *err = browser_last_error(ctx->session);
        return tool_result_error(err ? err : "evaluation failed",
                                 "execution_error");
    }
    ToolResult *tr = tool_result_create(json);
    free(json);
    return tr;
}

static ToolResult *tool_browser_screenshot(Tool *self, cJSON *args)
{
    ToolResult *open_result = tool_browser_ensure(self, args);
    if (open_result) return open_result;

    BrowserCtx *ctx = self->ctx;
    cJSON *path_j = cJSON_GetObjectItem(args, "path");
    char default_path[512];
    const char *path = cJSON_IsString(path_j) ? path_j->valuestring : NULL;
    if (!path || !path[0])
    {
        const char *tmp = getenv("TMPDIR");
        if (!tmp || !tmp[0]) tmp = "/tmp";
        int n = snprintf(default_path, sizeof(default_path),
                         "%s/echo_ai_screenshot_%ld_%ld.png", tmp,
                         (long)getpid(), (long)time(NULL));
        if (n <= 0 || (size_t)n >= sizeof(default_path))
            return tool_result_error("cannot build default screenshot path",
                                     "execution_error");
        path = default_path;
    }

    int ms = tool_browser_get_timeout(args);
    if (browser_screenshot(ctx->session, path, ms) != 0)
    {
        const char *err = browser_last_error(ctx->session);
        return tool_result_error(err ? err : "screenshot failed",
                                 "execution_error");
    }

    char out[512];
    int n = snprintf(out, sizeof(out),
                     "screenshot saved to %s (visible in the open "
                     "browser window)", path);
    if (n <= 0 || (size_t)n >= sizeof(out))
        return tool_result_error("screenshot saved", "execution_error");
    return tool_result_create(out);
}

/* Must track browser.h's browser_click validation: the tool reports bad
 * button names as a validation_error so the LLM can self-correct against
 * the vocabulary, while the browser layer is the hard CDP-enum guarantee.
 * Return: 1 = accepted (out_button set, or NULL for the "left" default),
 * 0 = present but not a CDP MouseButton value, -1 = present but not a
 * string at all. */
static int parse_click_button(cJSON *args, const char **out_button)
{
    cJSON *b_j = cJSON_GetObjectItem(args, "button");
    *out_button = NULL;
    if (!b_j) return 1; /* absent: default left */
    if (!cJSON_IsString(b_j)) return -1;

    static const char *const allowed[] = {
        "left", "middle", "right", "back", "forward",
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        if (strcmp(b_j->valuestring, allowed[i]) == 0)
        {
            *out_button = b_j->valuestring;
            return 1;
        }
    return 0;
}

static ToolResult *tool_browser_click(Tool *self, cJSON *args)
{
    ToolResult *open_result = tool_browser_ensure(self, args);
    if (open_result) return open_result;

    BrowserCtx *ctx = self->ctx;
    cJSON *x_j = cJSON_GetObjectItem(args, "x");
    cJSON *y_j = cJSON_GetObjectItem(args, "y");
    if (!cJSON_IsNumber(x_j) || !cJSON_IsNumber(y_j))
        return tool_result_error("click requires numeric 'x' and 'y' "
                                 "viewport coordinates",
                                 "validation_error");

    const char *button = NULL;
    int bc = parse_click_button(args, &button);
    if (bc <= 0)
        return tool_result_error(bc < 0
            ? "button must be a string (left, middle, right, back or "
              "forward)"
            : "unsupported button (expected left, middle, right, back or "
              "forward)",
            "validation_error");

    int x = (int)x_j->valuedouble;
    int y = (int)y_j->valuedouble;
    int ms = tool_browser_get_timeout(args);
    if (browser_click(ctx->session, x, y, button, ms) != 0)
    {
        const char *err = browser_last_error(ctx->session);
        return tool_result_error(err ? err : "click failed",
                                 "execution_error");
    }

    char out[160];
    int n = snprintf(out, sizeof(out),
                     "native %s click sent at viewport (%d, %d)",
                     button ? button : "left", x, y);
    if (n <= 0 || (size_t)n >= sizeof(out))
        return tool_result_error("native click sent", "execution_error");
    return tool_result_create(out);
}

static ToolResult *tool_browser_status(Tool *self)
{
    BrowserCtx *ctx = self->ctx;
    if (!ctx->session || !browser_is_open(ctx->session))
        return tool_result_create("browser not running");
    return tool_result_create("browser running (visible window open)");
}

static ToolResult *tool_browser_close(Tool *self)
{
    BrowserCtx *ctx = self->ctx;
    if (!ctx->session) return tool_result_create("browser not running");
    browser_close(ctx->session);
    ctx->session = NULL;
    return tool_result_create("browser window closed");
}

static ToolResult *tool_browser_execute(Tool *self, const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args)
        return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *action_j = cJSON_GetObjectItem(args, "action");
    if (!cJSON_IsString(action_j))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'action' argument",
                                 "validation_error");
    }
    const char *action = action_j->valuestring;

    ToolResult *result = NULL;
    if (strcmp(action, "open") == 0)
        result = tool_browser_open(self, args);
    else if (strcmp(action, "navigate") == 0)
        result = tool_browser_navigate(self, args);
    else if (strcmp(action, "evaluate") == 0)
        result = tool_browser_evaluate(self, args);
    else if (strcmp(action, "screenshot") == 0)
        result = tool_browser_screenshot(self, args);
    else if (strcmp(action, "click") == 0)
        result = tool_browser_click(self, args);
    else if (strcmp(action, "status") == 0)
        result = tool_browser_status(self);
    else if (strcmp(action, "close") == 0)
        result = tool_browser_close(self);
    else
    {
        const char *list = "open, navigate, evaluate, screenshot, "
                           "click, status, close";
        char msg[128];
        int n = snprintf(msg, sizeof(msg),
                         "unknown action '%s' (expected one of: %s)",
                         action, list);
        result = tool_result_error(
            n > 0 && (size_t)n < sizeof(msg) ? msg : "unknown action",
            "validation_error");
    }

    cJSON_Delete(args);
    return result;
}

static void tool_browser_destroy(Tool *self)
{
    if (!self) return;
    BrowserCtx *ctx = self->ctx;
    if (ctx && ctx->session) browser_close(ctx->session);
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_browser_create - construct the browser tool
 * @safety: borrowed SafetyConfig; not used yet (a spawned browser
 * cannot be socket-policy-checked), kept for signature consistency.
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy().
 */
Tool *tool_browser_create(SafetyConfig *safety)
{
    (void)safety;
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    BrowserCtx *ctx = calloc(1, sizeof(BrowserCtx));
    if (!ctx)
    {
        free(t);
        return NULL;
    }

    t->name = str_dup("browser");
    t->description = str_dup(
        "Open a visible desktop browser window and control it: navigate "
        "to pages, read page text, run JavaScript, take screenshots, "
        "and send native (trusted) mouse clicks. The browser is a real "
        "Chromium-family browser (auto-discovered; override with "
        "ECHO_BROWSER_BIN or the 'binary' argument) and the user can "
        "watch every action in the window.");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"action\":{\"type\":\"string\",\"enum\":[\"open\",\"navigate\","
        "\"evaluate\",\"screenshot\",\"click\",\"status\",\"close\"],"
        "\"description\":\"what to do\"},"
        "\"url\":{\"type\":\"string\",\"description\":\"http(s) URL for "
        "navigate\"},"
        "\"expression\":{\"type\":\"string\",\"description\":\"JavaScript "
        "to run for evaluate; the result value is returned as JSON\"},"
        "\"path\":{\"type\":\"string\",\"description\":\"file to write "
        "the screenshot to (default: a temp file)\"},"
        "\"x\":{\"type\":\"integer\",\"description\":\"viewport x (CSS "
        "pixels) for the click action\"},"
        "\"y\":{\"type\":\"integer\",\"description\":\"viewport y (CSS "
        "pixels) for the click action\"},"
        "\"button\":{\"type\":\"string\",\"enum\":[\"left\",\"middle\","
        "\"right\",\"back\",\"forward\"],\"description\":\"mouse button "
        "for the click action (default left; right opens a context menu, "
        "middle opens a link in a new tab)\"},"
        "\"max_chars\":{\"type\":\"integer\",\"description\":\"page text "
        "cap (default 25000)\"},"
        "\"timeout\":{\"type\":\"number\",\"description\":\"seconds to "
        "wait before giving up (default 30)\"},"
        "\"binary\":{\"type\":\"string\",\"description\":\"browser "
        "executable path or name; default auto-discovery\"}"
        "},\"required\":[\"action\"]}"
    );
    t->ctx = ctx; /* attach before the failure check so destroy() frees it */
    if (!t->name || !t->description || !t->parameters_schema)
    {
        tool_browser_destroy(t);
        return NULL;
    }
    t->execute = tool_browser_execute;
    t->destroy = tool_browser_destroy;
    return t;
}

/**
 * tool_browser_set_headless - set the window mode for future opens
 * @self: tool.
 * @headless: 1 = run without a window, 0 = visible window (default).
 *
 * Configured once at startup from the browser.headless config key
 * (registry_set_browser_headless); the mode applies to every session
 * this tool opens. Opening an already-running session is unaffected.
 */
void tool_browser_set_headless(Tool *self, int headless)
{
    if (!self || !self->ctx) return;
    BrowserCtx *ctx = self->ctx;
    ctx->headless = headless ? 1 : 0;
}

#ifdef TOOL_BROWSER_TEST
/**
 * tool_browser_test_set_session - inject a session (test-only)
 * @self: tool.
 * @session: session the tool should drive; not owned by the tool.
 *
 * Lets tests exercise every action against a fake CDP peer without
 * spawning a browser. browser_close() on an injected session is a
 * no-op for the kill/profile parts, but the tool still NULLs its
 * pointer; the test owns the session.
 */
void tool_browser_test_set_session(Tool *self, BrowserSession *session)
{
    if (!self || !self->ctx) return;
    BrowserCtx *ctx = self->ctx;
    ctx->session = session;
}
#endif
