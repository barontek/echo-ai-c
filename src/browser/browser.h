/*
 * browser.h - Headful/headless Chromium-family browser automation:
 * spawns any CDP-capable browser (Brave, Chrome, Edge, Chromium, ...)
 * over --remote-debugging-pipe and drives it through the shared cdp
 * transport. web_fetch's JS-challenge fallback can reuse
 * browser_fetch_text() instead of shelling out to curl-impersonate.
 * Depends on: cdp.h, cJSON, pthreads.
 */

#ifndef ECHO_BROWSER_H
#define ECHO_BROWSER_H

#include <stddef.h>

typedef struct BrowserSession BrowserSession;
typedef struct CdpClient CdpClient;

/**
 * BrowserConfig - how to launch the browser.
 *
 * Every field is optional; NULL/0 picks the default, so callers can
 * pass a zeroed struct for a plain visible-browser session.
 */
typedef struct {
    const char *binary;      /* explicit path or name; NULL = discover:
                                ECHO_BROWSER_BIN, then $BROWSER, then a
                                per-platform candidate list */
    int headless;            /* 0 = visible window on the user's desktop
                                (default); 1 = --headless=new */
    const char *profile_dir; /* user data dir; NULL = fresh temp dir that
                                is deleted on browser_close() */
    const char *extra_flags; /* space-separated extra args appended to the
                                command line, e.g. "--no-sandbox
                                --window-size=1440,900" */
    int timeout_ms;          /* default per-call timeout; 0 = 30000 */
} BrowserConfig;

/**
 * BrowserPage - one successfully read page.
 * @title, @url, @text: caller-owned strings, free each with free();
 * text is the page innerText truncated to the requested max_chars.
 */
typedef struct {
    char *title;
    char *url;
    char *text;
} BrowserPage;

/**
 * browser_open - spawn a browser and handshake over CDP
 * @cfg: launch configuration; NULL is treated as a zeroed config
 *   (visible window, auto-discovered binary, temp profile).
 * @err_out: optional out-param receiving a caller-owned failure
 *   message on error (NULL and untouched on success); free with
 *   free(). On OOM the message may be NULL.
 *
 * Spawns the discovered binary with --remote-debugging-pipe and waits
 * for a Browser.getVersion reply, so a successful return means the
 * session is usable. The browser window is visible unless headless is
 * requested; the process is a child of the caller and is reaped by
 * browser_close().
 *
 * Return: caller-owned BrowserSession (release with browser_close()),
 * or NULL on failure. The returned session is not thread-safe:
 * serialize all calls on it.
 */
BrowserSession *browser_open(const BrowserConfig *cfg, char **err_out);

/**
 * browser_close - kill the browser, remove the temp profile, free all
 * state
 * @s: session, or NULL (no-op).
 *
 * Kills the child (SIGKILL after a brief SIGTERM grace), waits for it,
 * joins the CDP transport, removes the mkdtemp'd profile dir unless a
 * caller-provided profile was used, and frees the session. The session
 * pointer is invalid afterwards.
 *
 * Return: void; never fails.
 */
void browser_close(BrowserSession *s);

/**
 * browser_navigate - open a URL in the session's page and read it
 * @s: session from browser_open().
 * @url: http:// or https:// URL; other schemes are rejected.
 * @max_chars: byte cap for out->text (0 = 25000).
 * @timeout_ms: per-command timeout; 0 = session default.
 * @out: zeroed BrowserPage receiving owned title/url/text; caller frees
 *   each string (see browser_page_free()).
 *
 * Navigates (reusing the existing tab on subsequent calls), waits for
 * the load event, then extracts title, final URL and innerText. A page
 * that never finishes loading is read anyway with a "still loading"
 * note in the text.
 *
 * Return: 0 on success (out filled), -1 on failure (out untouched,
 * reason in browser_last_error()).
 */
int browser_navigate(BrowserSession *s, const char *url, size_t max_chars,
                     int timeout_ms, BrowserPage *out);

/**
 * browser_page_free - release a BrowserPage's owned strings
 * @p: page, or NULL (no-op). The struct itself is caller-owned.
 *
 * Frees title, url and text and NULLs them.
 *
 * Return: void; never fails.
 */
void browser_page_free(BrowserPage *p);

/**
 * browser_evaluate - run JavaScript in the page
 * @s: session.
 * @expression: JS expression to evaluate (any script string; an
 *   IIFE returning a value is the idiomatic way to produce one).
 * @out_json: receives caller-owned compact JSON of the expression's
 *   result value (a string comes back JSON-encoded, an object comes
 *   back as an object); free with free().
 * @timeout_ms: per-command timeout; 0 = session default.
 *
 * Promises are awaited (awaitPromise). A JS exception or a promise
 * rejection fails the call with the exception message.
 *
 * Return: 0 on success, -1 on failure (reason in browser_last_error()).
 */
int browser_evaluate(BrowserSession *s, const char *expression,
                     char **out_json, int timeout_ms);

/**
 * browser_screenshot - capture the visible page to a PNG file
 * @s: session.
 * @path: file to write; the file is created/truncated (0644).
 * @timeout_ms: per-command timeout; 0 = session default.
 *
 * Return: 0 on success, -1 on failure (reason in browser_last_error()).
 */
int browser_screenshot(BrowserSession *s, const char *path, int timeout_ms);

/**
 * browser_fetch_text - navigate and return the page text
 * @s: session.
 * @url: http(s) URL.
 * @max_chars: byte cap for the returned text (0 = 25000).
 * @timeout_ms: per-command timeout; 0 = session default.
 *
 * Convenience for non-tool callers (web_fetch's JS-challenge layer):
 * a "title\n\nbody" style text blob, truncated to max_chars. Returns
 * NULL on failure with the reason in browser_last_error().
 *
 * Return: caller-owned text (free with free()), or NULL on failure.
 */
char *browser_fetch_text(BrowserSession *s, const char *url,
                         size_t max_chars, int timeout_ms);

/**
 * browser_is_open - is the session alive?
 * @s: session, or NULL.
 *
 * Return: 1 while the transport is up, 0 after close or a browser
 * death. Never fails.
 */
int browser_is_open(BrowserSession *s);

/**
 * browser_last_error - reason for the most recent failure
 * @s: session.
 *
 * Return: borrowed, NUL-terminated message; NULL if no failure since
 * the last success (or NULL session). Valid until the next call on the
 * session; do not free.
 */
const char *browser_last_error(BrowserSession *s);

#ifdef BROWSER_TEST
/**
 * browser_test_attach - wrap an existing CDP client as a session
 * @c: client the session drives; NOT owned by the session (the caller
 *   keeps ownership and must close it itself after browser_close()).
 *
 * Test-only: lets unit tests drive the full navigate/evaluate/screenshot
 * stack against a fake CDP peer without spawning a browser process.
 * The returned session has no pid and no profile dir; browser_close()
 * skips killing/removing those.
 *
 * Return: caller-owned BrowserSession, or NULL on OOM.
 */
BrowserSession *browser_test_attach(CdpClient *c);

/**
 * browser_test_set_strdup_fail - arm the alloc-failure hook
 * @nth_allocation: 1-based index of the str_dup that should fail; -1
 *   disarms. Counter resets on every call.
 *
 * Test-only fault injection (see AGENTS.md).
 */
void browser_test_set_strdup_fail(int nth_allocation);

/**
 * browser_test_decode_base64 - decode standard base64 (CDP screenshot
 * data) into raw bytes
 * @in: NUL-terminated base64; whitespace tolerated.
 * @out_len: receives decoded byte count.
 *
 * Test-only exposure of the screenshot decode path.
 *
 * Return: caller-owned bytes (free with free()), NULL on bad input.
 */
unsigned char *browser_test_decode_base64(const char *in, size_t *out_len);

/**
 * browser_test_remove_profile_dir - run profile removal with retries
 * @dir: directory to remove (must exist).
 *
 * Test-only: regression hook for the close race where a straggler
 * process kept writing profile files while rm ran, failing the first
 * attempt with "Directory not empty".
 */
void browser_test_remove_profile_dir(const char *dir);
#endif

#endif
