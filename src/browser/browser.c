/*
 * browser.c - Chromium-family browser automation. Resolves a browser
 * binary without hardcoding one (env vars first, then a per-platform
 * candidate list), spawns it with --remote-debugging-pipe, and drives
 * page navigation/evaluation/screenshots through the shared cdp
 * transport. Kept browser-agnostic so web_fetch can reuse
 * browser_fetch_text() for JS-challenge pages. Depends on: browser.h,
 * cdp.h, cJSON, string_utils, logging, pthreads.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "browser.h"
#include "cdp.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

#ifdef BROWSER_TEST
static int bstrdup_fail_at = -1;
static int bstrdup_call_count = 0;

static char *browser_test_strdup(const char *s)
{
    bstrdup_call_count++;
    if (bstrdup_call_count == bstrdup_fail_at) return NULL;
    return str_dup(s);
}

#define str_dup browser_test_strdup
#endif

#define DEFAULT_TIMEOUT_MS 30000
#define DEFAULT_MAX_CHARS 25000
/* Extraction, readyState polling and screenshots all work on the
 * visible tab; a URL cap keeps clearly-wrong tool input cheap to
 * reject before anything is spawned. */
#define URL_MAX_LEN 4096

struct BrowserSession {
    CdpClient *client;      /* owned, unless external_client */
    pid_t pid;              /* 0 for test-attached sessions */
    char *binary;           /* owned; resolved executable path */
    char *profile_dir;      /* owned; NULL when none */
    int profile_owned;      /* 1 = mkdtemp'd by us, remove at close */
    char *target_id;        /* owned; current page target */
    char *session_id;       /* owned; CDP session for that target */
    int timeout_ms;
    char *last_error;       /* owned */
    int external_client;    /* test attach: no kill/close/profile */
};

/* ---- errors ------------------------------------------------------ */

static void set_error(BrowserSession *s, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); // NOLINT(cert-err33-c)
    va_end(ap);

    free(s->last_error);
    s->last_error = str_dup(buf);
    /* OOM leaves last_error NULL; callers must tolerate that. */
}

/* ---- base64 (screenshot data) ------------------------------------
 * CDP returns Page.captureScreenshot data as standard base64; OpenSSL's
 * EVP_DecodeBlock pads return-lengths ambiguously, so hand-roll it the
 * same way oauth_codec.c hand-rolls base64url. */

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned char *decode_base64(const char *in, size_t *out_len)
{
    if (!in || !out_len) return NULL;

    size_t len = strlen(in);
    char *clean = malloc(len + 1);
    if (!clean) return NULL;
    size_t cl = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (in[i] == ' ' || in[i] == '\n' || in[i] == '\r' || in[i] == '\t')
            continue;
        clean[cl++] = in[i];
    }
    clean[cl] = '\0';

    if (cl == 0 || cl % 4 != 0)
    {
        free(clean);
        return NULL;
    }
    size_t pad = 0;
    while (pad < cl && clean[cl - 1 - pad] == '=') pad++;
    if (pad > 2)
    {
        free(clean);
        return NULL;
    }

    size_t out_cap = cl / 4 * 3;
    unsigned char *out = malloc(out_cap ? out_cap : 1);
    if (!out)
    {
        free(clean);
        return NULL;
    }
    size_t oi = 0;
    for (size_t i = 0; i < cl; i += 4)
    {
        int a = b64_value(clean[i]);
        int b = b64_value(clean[i + 1]);
        int c = clean[i + 2] == '=' ? 0 : b64_value(clean[i + 2]);
        int d = clean[i + 3] == '=' ? 0 : b64_value(clean[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0)
        {
            free(out);
            free(clean);
            return NULL;
        }
        out[oi++] = (unsigned char)((a << 2) | (b >> 4));
        if (clean[i + 2] != '=')
            out[oi++] = (unsigned char)(((b & 0x0F) << 4) | (c >> 2));
        if (clean[i + 3] != '=')
            out[oi++] = (unsigned char)(((c & 0x03) << 6) | d);
    }
    free(clean);
    *out_len = oi;
    return out;
}

/* ---- binary discovery --------------------------------------------
 * Order: explicit arg, ECHO_BROWSER_BIN, $BROWSER, then a per-platform
 * candidate list. Nothing here hardcodes a single browser; the list is
 * only a fallback for unconfigured machines. */

static char *path_lookup(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || !name || !name[0]) return NULL;

    char *copy = str_dup(path);
    if (!copy) return NULL;
    char *result = NULL;
    char *save = NULL;
    char *tok = strtok_r(copy, ":", &save);
    while (tok && !result)
    {
        char full[4096];
        int n = snprintf(full, sizeof(full), "%s/%s", tok, name);
        if (n > 0 && (size_t)n < sizeof(full) && access(full, X_OK) == 0)
            result = str_dup(full);
        tok = strtok_r(NULL, ":", &save);
    }
    free(copy);
    return result;
}

static const char *candidate_names[] = {
    "brave", "brave-browser", "google-chrome", "google-chrome-stable",
    "chromium", "chromium-browser", "microsoft-edge",
    "microsoft-edge-stable", "msedge", "chrome", "opera", "vivaldi", NULL
};

#ifdef __APPLE__
static const char *candidate_bundles[] = {
    "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "/Applications/Opera.app/Contents/MacOS/Opera",
    "/Applications/Vivaldi.app/Contents/MacOS/Vivaldi",
    NULL
};
#endif

/* Escape hatch for hermetic tests and strict-config users: when set,
 * only explicit configuration (binary argument, ECHO_BROWSER_BIN,
 * $BROWSER) is honored — the PATH-name and macOS-bundle candidate
 * lists are skipped. Without it, discovery "succeeds" on any machine
 * with a browser installed (e.g. CI runners) even when the test asked
 * for nothing to be configured. */
static int fallback_disabled(void)
{
    const char *v = getenv("ECHO_BROWSER_NO_FALLBACK");
    return v && v[0];
}

static char *resolve_binary(const char *explicit)
{
    if (explicit && explicit[0])
    {
        if (strchr(explicit, '/'))
            return access(explicit, X_OK) == 0 ? str_dup(explicit) : NULL;
        return path_lookup(explicit);
    }

    const char *env = getenv("ECHO_BROWSER_BIN");
    if (env && env[0])
    {
        char *found = strchr(env, '/') ? (access(env, X_OK) == 0 ? str_dup(env) : NULL)
                                       : path_lookup(env);
        if (found) return found;
    }
    env = getenv("BROWSER");
    if (env && env[0])
    {
        char *found = strchr(env, '/') ? (access(env, X_OK) == 0 ? str_dup(env) : NULL)
                                       : path_lookup(env);
        if (found) return found;
    }
    if (fallback_disabled()) return NULL;

    for (int i = 0; candidate_names[i]; i++)
    {
        char *found = path_lookup(candidate_names[i]);
        if (found) return found;
    }
#ifdef __APPLE__
    for (int i = 0; candidate_bundles[i]; i++)
    {
        if (access(candidate_bundles[i], X_OK) == 0)
            return str_dup(candidate_bundles[i]);
    }
#endif
    return NULL;
}

/* ---- spawn ------------------------------------------------------- */

static char *make_temp_profile(void)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    char tpl[4096];
    int n = snprintf(tpl, sizeof(tpl), "%s/echo-ai-browser-XXXXXX", tmp);
    if (n <= 0 || (size_t)n >= sizeof(tpl)) return NULL;
    if (!mkdtemp(tpl)) return NULL;
    return str_dup(tpl);
}

/* Build the child argv. All strings are freshly allocated; the caller
 * frees every entry and the array itself with free_argv(). */
static char **build_argv(BrowserSession *s, int headless,
                         const char *extra, size_t *out_count)
{
    char *extra_copy = extra ? str_dup(extra) : NULL;
    if (extra && !extra_copy)
    {
        set_error(s, "OOM building browser arguments");
        return NULL;
    }

    int tokens = 0;
    if (extra_copy)
    {
        char *save = NULL;
        char *tok = strtok_r(extra_copy, " \t", &save);
        while (tok)
        {
            tokens++;
            tok = strtok_r(NULL, " \t", &save);
        }
    }

    size_t cap = (size_t)5 + (size_t)tokens + 1;
    char **argv = calloc(cap, sizeof(char *));
    if (!argv)
    {
        free(extra_copy);
        set_error(s, "OOM building browser arguments");
        return NULL;
    }

    size_t i = 0;
    argv[i++] = s->binary; /* borrowed; never freed with the array */
    argv[i++] = str_dup("--remote-debugging-pipe");
    if (!argv[i - 1]) goto oom;

    char profile_arg[4200];
    int n = snprintf(profile_arg, sizeof(profile_arg),
                     "--user-data-dir=%s", s->profile_dir);
    if (n <= 0 || (size_t)n >= sizeof(profile_arg))
    {
        set_error(s, "profile path too long");
        goto oom;
    }
    argv[i++] = str_dup(profile_arg);
    if (!argv[i - 1]) goto oom;
    argv[i++] = str_dup("--no-first-run");
    if (!argv[i - 1]) goto oom;
    argv[i++] = str_dup("--no-default-browser-check");
    if (!argv[i - 1]) goto oom;
    if (headless)
    {
        argv[i++] = str_dup("--headless=new");
        if (!argv[i - 1]) goto oom;
    }

    if (extra_copy)
    {
        char *save = NULL;
        char *tok = strtok_r(extra_copy, " \t", &save);
        while (tok)
        {
            argv[i++] = str_dup(tok); // NOLINT(clang-analyzer-security.ArrayBound)
            if (!argv[i - 1]) goto oom;
            tok = strtok_r(NULL, " \t", &save);
        }
    }
    argv[i] = NULL; // NOLINT(clang-analyzer-security.ArrayBound)
    free(extra_copy);
    *out_count = i + 1;
    return argv;

oom:
    free(extra_copy);
    for (size_t j = 0; j < i; j++)
    {
        if (argv[j] != s->binary) free(argv[j]);
    }
    free(argv);
    set_error(s, "OOM building browser arguments");
    return NULL;
}

/* Remove the temp profile with rm(1): hand-rolled recursion over a
 * browser profile (symlinks, deep nesting) is more code and more error
 * paths than the two-line child. The dir is ours, so this is safe.
 * Retries: even after the group kill, a straggler process can still be
 * writing files while rm runs, which made the first attempt fail with
 * "Directory not empty" in real use. rm's stderr is silenced; the
 * parent logs one warning only if the dir survives every attempt. */
static void remove_profile_dir(const char *dir)
{
    for (int attempt = 0; attempt < 20; attempt++)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0)
            {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            execlp("rm", "rm", "-rf", "--", dir, (char *)NULL);
            _exit(127);
        }
        if (pid > 0)
        {
            int status = 0;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                return;
        }
        usleep(100000);
    }
    log_warn("browser temp profile not fully removed",
             "dir", dir, NULL);
}

/* Free an argv array built by build_argv. Entry 0 is always the
 * borrowed binary pointer and is skipped, matching build_argv's
 * contract. */
static void free_argv(char **argv, size_t count)
{
    if (!argv) return;
    for (size_t j = 1; j < count; j++)
    {
        if (argv[j] != NULL) free(argv[j]);
    }
    free(argv);
}

static int spawn_browser(BrowserSession *s, int headless,
                         const char *extra, int *write_fd, int *read_fd)
{
    size_t argv_count = 0;
    char **argv = build_argv(s, headless, extra, &argv_count);
    if (!argv) return -1;

    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0)
    {
        set_error(s, "pipe: %s", strerror(errno));
        goto fail_free_argv;
    }
    if (pipe(out_pipe) != 0)
    {
        set_error(s, "pipe: %s", strerror(errno));
        close(in_pipe[0]);
        close(in_pipe[1]);
        goto fail_free_argv;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        set_error(s, "fork: %s", strerror(errno));
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        goto fail_free_argv;
    }

    if (pid == 0)
    {
        /* Child: CDP lives on fd 3 (commands in) and fd 4 (messages
         * out); everything else is closed after redirecting stdio to
         * /dev/null so browser chatter never pollutes the TUI.
         * pipe() may have handed back fd 3/4 themselves, so each source
         * is moved to a spare fd before dup2'ing — close-then-dup2
         * order bugs here are why a browser can see a closed pipe at
         * launch and refuse to start. */
        close(in_pipe[1]);
        close(out_pipe[0]);

        int sources[2] = {in_pipe[0], out_pipe[1]};
        for (int i = 0; i < 2; i++)
        {
            if (sources[i] == 3 || sources[i] == 4)
            {
                int spare = fcntl(sources[i], F_DUPFD_CLOEXEC, 10);
                if (spare < 0) _exit(126);
                close(sources[i]);
                sources[i] = spare;
            }
        }
        for (int i = 0; i < 2; i++)
        {
            int target = i == 0 ? 3 : 4;
            if (dup2(sources[i], target) < 0) _exit(126);
            if (sources[i] != target) close(sources[i]);
        }

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0)
        {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            close(devnull);
        }
        for (int i = 5; i < 256; i++) close(i);

        /* Own process group: Chromium's renderer/GPU/utility children
         * inherit it, so a group signal at close reaches everything
         * that may still be writing to the profile dir. */
        (void)setpgid(0, 0);

        execv(s->binary, argv);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    s->pid = pid;
    *write_fd = in_pipe[1];
    *read_fd = out_pipe[0];
    free_argv(argv, argv_count);
    return 0;

fail_free_argv:
    free_argv(argv, argv_count);
    return -1;
}

/* Kill and reap the child, signalling its whole process group: killing
 * only the browser leader leaves renderer/GPU children alive, and they
 * keep writing to the profile dir for a while (see remove_profile_dir
 * and https://crbug.com/40259890-adjacent teardown behavior). */
static void bury_browser(BrowserSession *s)
{
    if (s->pid <= 0) return;

    if (kill(-s->pid, SIGTERM) != 0 && errno == ESRCH)
        kill(s->pid, SIGTERM); /* group already gone: leader only */
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int waited = 0;
    int reaped = 0;
    while (waited < 3000)
    {
        int status = 0;
        pid_t r = waitpid(s->pid, &status, WNOHANG);
        if (r == s->pid)
        {
            reaped = 1;
            break;
        }
        if (r < 0 && errno != EINTR) break;
        usleep(50000);
        waited += 50;
    }
    if (!reaped)
    {
        if (kill(-s->pid, SIGKILL) != 0 && errno == ESRCH)
            kill(s->pid, SIGKILL);
        waitpid(s->pid, NULL, 0);
    }
    s->pid = 0;
}

/* ---- CDP helpers ------------------------------------------------- */

/* Send a command and return its parsed result object; the caller owns
 * the result (free with cJSON_Delete). On any failure, last_error is
 * set and NULL is returned. */
static cJSON *cdp_call_result(BrowserSession *s, const char *method,
                              cJSON *params, const char *session_id,
                              int timeout_ms)
{
    char *raw = cdp_client_call(s->client, method, params, session_id,
                                timeout_ms);
    if (!raw)
    {
        set_error(s, "%s: no response (browser exited or timed out)",
                  method);
        return NULL;
    }
    cJSON *msg = cJSON_Parse(raw);
    free(raw);
    if (!msg)
    {
        set_error(s, "%s: malformed response from browser", method);
        return NULL;
    }

    cJSON *err = cJSON_GetObjectItem(msg, "error");
    if (cJSON_IsObject(err))
    {
        const char *em = NULL;
        cJSON *m = cJSON_GetObjectItem(err, "message");
        if (cJSON_IsString(m)) em = m->valuestring;
        set_error(s, "%s: %s", method, em ? em : "protocol error");
        cJSON_Delete(msg);
        return NULL;
    }

    cJSON *result = cJSON_GetObjectItem(msg, "result");
    if (!cJSON_IsObject(result))
    {
        set_error(s, "%s: response missing result", method);
        cJSON_Delete(msg);
        return NULL;
    }
    cJSON_DetachItemFromObject(msg, "result");
    cJSON_Delete(msg);
    return result;
}

/* Create the page target and attach a flattened session to it, once.
 * Subsequent calls reuse the stored session id. */
static int browser_ensure_page(BrowserSession *s, int timeout_ms)
{
    if (s->session_id) return 0;

    cJSON *params = cJSON_CreateObject();
    if (!params)
    {
        set_error(s, "OOM");
        return -1;
    }
    cJSON_AddStringToObject(params, "url", "about:blank");
    cJSON *result = cdp_call_result(s, "Target.createTarget", params,
                                    NULL, timeout_ms);
    cJSON_Delete(params);
    if (!result) return -1;

    const char *tid = NULL;
    cJSON *tid_j = cJSON_GetObjectItem(result, "targetId");
    if (cJSON_IsString(tid_j)) tid = tid_j->valuestring;
    char *target_id = str_dup(tid ? tid : "");
    cJSON_Delete(result);
    if (!target_id)
    {
        set_error(s, "OOM");
        return -1;
    }

    params = cJSON_CreateObject();
    if (!params)
    {
        free(target_id);
        set_error(s, "OOM");
        return -1;
    }
    cJSON_AddStringToObject(params, "targetId", target_id);
    cJSON_AddBoolToObject(params, "flatten", 1);
    result = cdp_call_result(s, "Target.attachToTarget", params, NULL,
                             timeout_ms);
    cJSON_Delete(params);
    free(target_id);
    if (!result) return -1;

    const char *sid = NULL;
    cJSON *sid_j = cJSON_GetObjectItem(result, "sessionId");
    if (cJSON_IsString(sid_j)) sid = sid_j->valuestring;
    s->session_id = str_dup(sid ? sid : "");
    cJSON_Delete(result);
    if (!s->session_id)
    {
        set_error(s, "OOM");
        return -1;
    }
    return 0;
}

/* ---- public API -------------------------------------------------- */

/* Failure exit for browser_open: hand the collected last_error to the
 * caller (who would otherwise lose it when the session is freed) and
 * release the session. */
static void browser_open_fail(BrowserSession *s, char **err_out)
{
    if (err_out)
    {
        *err_out = str_dup(s->last_error ? s->last_error
                                         : "browser open failed");
    }
    browser_close(s);
}

/**
 * browser_open - see browser.h
 */
BrowserSession *browser_open(const BrowserConfig *cfg, char **err_out)
{
    if (err_out) *err_out = NULL;

    BrowserSession *s = calloc(1, sizeof(BrowserSession));
    if (!s)
    {
        if (err_out) *err_out = str_dup("OOM");
        return NULL;
    }
    s->timeout_ms = (cfg && cfg->timeout_ms > 0) ? cfg->timeout_ms
                                                 : DEFAULT_TIMEOUT_MS;

    s->binary = resolve_binary(cfg && cfg->binary ? cfg->binary : NULL);
    if (!s->binary)
    {
        set_error(s, "no Chromium-family browser found; set ECHO_BROWSER_BIN "
                     "to a Brave/Chrome/Edge/Chromium binary");
        browser_open_fail(s, err_out);
        return NULL;
    }

    const char *profile = cfg && cfg->profile_dir ? cfg->profile_dir : NULL;
    if (!profile) profile = getenv("ECHO_BROWSER_USER_DATA_DIR");
    if (profile && profile[0])
    {
        s->profile_dir = str_dup(profile);
        if (!s->profile_dir)
        {
            set_error(s, "OOM");
            browser_open_fail(s, err_out);
            return NULL;
        }
    }
    else
    {
        s->profile_dir = make_temp_profile();
        if (!s->profile_dir)
        {
            set_error(s, "failed to create a temp profile dir under "
                         "$TMPDIR");
            browser_open_fail(s, err_out);
            return NULL;
        }
        s->profile_owned = 1;
    }

    int headless = (cfg && cfg->headless) ? 1 : 0;
    const char *he = getenv("ECHO_BROWSER_HEADLESS");
    if (he && strcmp(he, "1") == 0) headless = 1;

    const char *extra = cfg && cfg->extra_flags ? cfg->extra_flags : NULL;
    if (!extra) extra = getenv("ECHO_BROWSER_FLAGS");

    int write_fd = -1;
    int read_fd = -1;
    if (spawn_browser(s, headless, extra, &write_fd, &read_fd) != 0)
    {
        browser_open_fail(s, err_out);
        return NULL;
    }

    s->client = cdp_client_new(write_fd, read_fd);
    if (!s->client)
    {
        set_error(s, "OOM starting CDP transport");
        bury_browser(s);
        close(write_fd);
        close(read_fd);
        browser_open_fail(s, err_out);
        return NULL;
    }

    /* Handshake: a Browser.getVersion reply proves the pipe transport
     * is live. Give the browser a little longer than a normal call. */
    int handshake_ms = s->timeout_ms > 15000 ? s->timeout_ms : 15000;
    cJSON *ver = cdp_call_result(s, "Browser.getVersion", NULL, NULL,
                                 handshake_ms);
    if (!ver)
    {
        if (cdp_client_is_dead(s->client))
        {
            int status = 0;
            if (waitpid(s->pid, &status, WNOHANG) == s->pid)
            {
                int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                set_error(s, "browser exited before CDP handshake "
                             "(exit %d); check ECHO_BROWSER_BIN and "
                             "ECHO_BROWSER_FLAGS", code);
            }
        }
        else
        {
            set_error(s, "browser did not answer the CDP handshake "
                         "within %d ms", handshake_ms);
        }
        browser_open_fail(s, err_out);
        return NULL;
    }
    cJSON_Delete(ver);

    log_info("browser opened", "binary", s->binary,
             "headless", headless ? "1" : "0", "profile", s->profile_dir,
             NULL);
    return s;
}

/**
 * browser_close - see browser.h
 */
void browser_close(BrowserSession *s)
{
    if (!s) return;

    bury_browser(s);
    if (s->client && !s->external_client)
    {
        cdp_client_close(s->client);
        s->client = NULL;
    }
    if (s->profile_owned && s->profile_dir)
        remove_profile_dir(s->profile_dir);

    free(s->binary);
    free(s->profile_dir);
    free(s->target_id);
    free(s->session_id);
    free(s->last_error);
    free(s);
}

/**
 * browser_navigate - see browser.h
 */
int browser_navigate(BrowserSession *s, const char *url, size_t max_chars,
                     int timeout_ms, BrowserPage *out)
{
    if (!s || !out) return -1;
    memset(out, 0, sizeof(*out));

    int ms = timeout_ms > 0 ? timeout_ms
                            : (s->timeout_ms > 0 ? s->timeout_ms
                                                 : DEFAULT_TIMEOUT_MS);
    size_t cap = max_chars > 0 ? max_chars : DEFAULT_MAX_CHARS;
    if (cap > 200000) cap = 200000;

    if (!url || !url[0])
    {
        set_error(s, "navigate: URL required");
        return -1;
    }
    if (strlen(url) > URL_MAX_LEN)
    {
        set_error(s, "navigate: URL too long");
        return -1;
    }
    if (!str_starts_with(url, "http://") && !str_starts_with(url, "https://"))
    {
        set_error(s, "navigate: unsupported scheme (http/https only), "
                     "got: %.64s", url);
        return -1;
    }
    if (browser_ensure_page(s, ms) != 0) return -1;

    cJSON *params = cJSON_CreateObject();
    if (!params)
    {
        set_error(s, "OOM");
        return -1;
    }
    cJSON_AddStringToObject(params, "url", url);
    cJSON *result = cdp_call_result(s, "Page.navigate", params,
                                    s->session_id, ms);
    cJSON_Delete(params);
    if (!result) return -1;

    const char *errtext = NULL;
    cJSON *et = cJSON_GetObjectItem(result, "errorText");
    if (cJSON_IsString(et)) errtext = et->valuestring;
    if (errtext && errtext[0])
    {
        set_error(s, "navigate: %s", errtext);
        cJSON_Delete(result);
        return -1;
    }
    cJSON_Delete(result);

    /* Wait for the load event by polling readyState. A page that never
     * finishes (streaming/SPA) is read anyway, marked as still
     * loading, rather than failing the whole call. */
    int done = 0;
    int waited = 0;
    while (waited < ms)
    {
        char *state = NULL;
        if (browser_evaluate(s, "document.readyState", &state,
                             ms - waited) == 0 && state)
        {
            /* state is the JSON-encoded value ("\"complete\""), so
             * parse it before comparing. */
            cJSON *st = cJSON_Parse(state);
            int complete = cJSON_IsString(st) &&
                           strcmp(st->valuestring, "complete") == 0;
            cJSON_Delete(st);
            free(state);
            if (complete)
            {
                done = 1;
                break;
            }
        }
        usleep(200000);
        waited += 200;
    }

    /* One evaluate that returns a JSON string of {t,u,x}; the slice
     * happens inside the page so only cap bytes ever leave it. */
    char expr[512];
    int n = snprintf(expr, sizeof(expr),
                     "JSON.stringify({t:document.title,"
                     "u:location.href,"
                     "x:(document.body?document.body.innerText:\"\")"
                     ".slice(0,%zu)})", cap);
    if (n <= 0 || (size_t)n >= sizeof(expr))
    {
        set_error(s, "navigate: expression too long");
        return -1;
    }

    char *encoded = NULL;
    if (browser_evaluate(s, expr, &encoded, ms) != 0 || !encoded)
        return -1;

    cJSON *str = cJSON_Parse(encoded);
    free(encoded);
    if (!cJSON_IsString(str))
    {
        cJSON_Delete(str);
        set_error(s, "navigate: unexpected page extraction result");
        return -1;
    }
    cJSON *inner = cJSON_Parse(str->valuestring);
    cJSON_Delete(str);
    if (!inner)
    {
        set_error(s, "navigate: page returned malformed extraction");
        return -1;
    }

    cJSON *t = cJSON_GetObjectItem(inner, "t");
    cJSON *u = cJSON_GetObjectItem(inner, "u");
    cJSON *x = cJSON_GetObjectItem(inner, "x");
    out->title = str_dup(cJSON_IsString(t) ? t->valuestring : "");
    out->url = str_dup(cJSON_IsString(u) ? u->valuestring : "");
    out->text = str_truncate_ellipsis_dup(cJSON_IsString(x) ? x->valuestring : "",
                                          cap);
    cJSON_Delete(inner);

    if (!out->title || !out->url || !out->text)
    {
        set_error(s, "OOM building page result");
        browser_page_free(out);
        return -1;
    }
    if (!done)
    {
        const char *note = "\n[note: page still loading when read]";
        char *grown = NULL;
        size_t need = strlen(out->text) + strlen(note) + 1;
        grown = malloc(need);
        if (grown)
        {
            snprintf(grown, need, "%s%s", out->text, note); // NOLINT(cert-err33-c)
            free(out->text);
            out->text = grown;
        }
    }
    return 0;
}

/**
 * browser_page_free - see browser.h
 */
void browser_page_free(BrowserPage *p)
{
    if (!p) return;
    free(p->title);
    free(p->url);
    free(p->text);
    memset(p, 0, sizeof(*p));
}

/**
 * browser_evaluate - see browser.h
 */
int browser_evaluate(BrowserSession *s, const char *expression,
                     char **out_json, int timeout_ms)
{
    if (!s || !out_json) return -1;
    *out_json = NULL;

    int ms = timeout_ms > 0 ? timeout_ms
                            : (s->timeout_ms > 0 ? s->timeout_ms
                                                 : DEFAULT_TIMEOUT_MS);
    if (!expression || !expression[0])
    {
        set_error(s, "evaluate: expression required");
        return -1;
    }
    if (browser_ensure_page(s, ms) != 0) return -1;

    cJSON *params = cJSON_CreateObject();
    if (!params)
    {
        set_error(s, "OOM");
        return -1;
    }
    cJSON_AddStringToObject(params, "expression", expression);
    cJSON_AddBoolToObject(params, "returnByValue", 1);
    cJSON_AddBoolToObject(params, "awaitPromise", 1);
    cJSON *result = cdp_call_result(s, "Runtime.evaluate", params,
                                    s->session_id, ms);
    cJSON_Delete(params);
    if (!result) return -1;

    cJSON *exc = cJSON_GetObjectItem(result, "exceptionDetails");
    if (cJSON_IsObject(exc))
    {
        const char *text = "JS exception";
        cJSON *t = cJSON_GetObjectItem(exc, "text");
        if (cJSON_IsString(t)) text = t->valuestring;
        const char *desc = NULL;
        cJSON *eo = cJSON_GetObjectItem(exc, "exception");
        cJSON *d = cJSON_IsObject(eo) ? cJSON_GetObjectItem(eo, "description") : NULL;
        if (cJSON_IsString(d)) desc = d->valuestring; // NOLINT(clang-analyzer-core.NullDereference)
        set_error(s, "evaluate: %s%s%s", text, desc ? ": " : "",
                  desc ? desc : "");
        cJSON_Delete(result);
        return -1;
    }

    cJSON *res = cJSON_GetObjectItem(result, "result");
    cJSON *value = cJSON_IsObject(res) ? cJSON_GetObjectItem(res, "value") : NULL;
    if (!value)
    {
        set_error(s, "evaluate: no result value returned");
        cJSON_Delete(result);
        return -1;
    }
    char *out = cJSON_PrintUnformatted(value);
    cJSON_Delete(result);
    if (!out)
    {
        set_error(s, "evaluate: OOM");
        return -1;
    }
    *out_json = out;
    return 0;
}

/**
 * browser_screenshot - see browser.h
 */
int browser_screenshot(BrowserSession *s, const char *path, int timeout_ms)
{
    if (!s) return -1;

    int ms = timeout_ms > 0 ? timeout_ms
                            : (s->timeout_ms > 0 ? s->timeout_ms
                                                 : DEFAULT_TIMEOUT_MS);
    if (!path || !path[0])
    {
        set_error(s, "screenshot: path required");
        return -1;
    }
    if (browser_ensure_page(s, ms) != 0) return -1;

    cJSON *params = cJSON_CreateObject();
    if (!params)
    {
        set_error(s, "OOM");
        return -1;
    }
    cJSON_AddStringToObject(params, "format", "png");
    cJSON *result = cdp_call_result(s, "Page.captureScreenshot", params,
                                    s->session_id, ms);
    cJSON_Delete(params);
    if (!result) return -1;

    const char *data = NULL;
    cJSON *d = cJSON_GetObjectItem(result, "data");
    if (cJSON_IsString(d)) data = d->valuestring;
    size_t len = 0;
    unsigned char *bytes = data ? decode_base64(data, &len) : NULL;
    cJSON_Delete(result);
    if (!bytes)
    {
        set_error(s, "screenshot: no image data returned by the browser");
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f)
    {
        set_error(s, "screenshot: cannot write %s: %s", path,
                  strerror(errno));
        free(bytes);
        return -1;
    }
    size_t written = fwrite(bytes, 1, len, f);
    int ferr = fclose(f);
    free(bytes);
    if (written != len || ferr != 0)
    {
        set_error(s, "screenshot: short write to %s", path);
        return -1;
    }
    return 0;
}

/* CDP Input.dispatchMouseEvent only dispatches a real button-press for
 * these MouseButton enum values; "none" (used for mouseMoved) press+release
 * would be a silent no-op click, so it is rejected like any unknown value. */
static int valid_click_button(const char *button)
{
    static const char *const allowed[] = {
        "left", "middle", "right", "back", "forward",
    };
    if (!button) return 0;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        if (strcmp(button, allowed[i]) == 0) return 1;
    return 0;
}

/**
 * browser_click - see browser.h
 */
int browser_click(BrowserSession *s, int x, int y, const char *button,
                  int timeout_ms)
{
    if (!s) return -1;

    if (!button || !button[0]) button = "left";
    else if (!valid_click_button(button))
    {
        set_error(s, "click: unsupported mouse button '%s' (expected "
                     "left, middle, right, back or forward)", button);
        return -1;
    }

    int ms = timeout_ms > 0 ? timeout_ms
                            : (s->timeout_ms > 0 ? s->timeout_ms
                                                 : DEFAULT_TIMEOUT_MS);
    if (browser_ensure_page(s, ms) != 0) return -1;

    /* mousePressed then mouseReleased at the same point = a click.
     * These are trusted browser-level input events (isTrusted=true),
     * unlike page-JS synthetic clicks, so anti-bot widgets that ignore
     * scripted clicks will still respond to them. */
    cJSON *params = cJSON_CreateObject();
    if (!params)
    {
        set_error(s, "OOM");
        return -1;
    }
    cJSON_AddStringToObject(params, "type", "mousePressed");
    cJSON_AddNumberToObject(params, "x", x);
    cJSON_AddNumberToObject(params, "y", y);
    /* CDP names button events with the MouseButton string enum; a numeric
     * button (as for the `buttons` bitset) is rejected with "Invalid
     * parameters". */
    cJSON_AddStringToObject(params, "button", button);
    cJSON_AddNumberToObject(params, "clickCount", 1);
    cJSON *result = cdp_call_result(s, "Input.dispatchMouseEvent", params,
                                    s->session_id, ms);
    cJSON_Delete(params);
    if (!result) return -1;
    cJSON_Delete(result);

    params = cJSON_CreateObject();
    if (!params)
    {
        set_error(s, "OOM");
        return -1;
    }
    cJSON_AddStringToObject(params, "type", "mouseReleased");
    cJSON_AddNumberToObject(params, "x", x);
    cJSON_AddNumberToObject(params, "y", y);
    cJSON_AddStringToObject(params, "button", button);
    cJSON_AddNumberToObject(params, "clickCount", 1);
    result = cdp_call_result(s, "Input.dispatchMouseEvent", params,
                             s->session_id, ms);
    cJSON_Delete(params);
    if (!result) return -1;
    cJSON_Delete(result);

    return 0;
}

/**
 * browser_fetch_text - see browser.h
 */
char *browser_fetch_text(BrowserSession *s, const char *url,
                         size_t max_chars, int timeout_ms)
{
    if (!s) return NULL;

    BrowserPage page = {0};
    if (browser_navigate(s, url, max_chars, timeout_ms, &page) != 0)
        return NULL;

    char head[4200];
    int n = snprintf(head, sizeof(head), "Title: %s\nURL: %s\n\n",
                     page.title ? page.title : "",
                     page.url ? page.url : "");
    if (n <= 0 || (size_t)n >= sizeof(head))
    {
        set_error(s, "fetch_text: page metadata too long");
        browser_page_free(&page);
        return NULL;
    }
    char *out = str_dup(head);
    if (out && page.text)
    {
        if (str_append(&out, page.text) != 0)
        {
            free(out);
            out = NULL;
        }
    }
    browser_page_free(&page);
    if (!out) set_error(s, "fetch_text: OOM");
    return out;
}

/**
 * browser_is_open - see browser.h
 */
int browser_is_open(BrowserSession *s)
{
    if (!s || !s->client) return 0;
    return !cdp_client_is_dead(s->client);
}

/**
 * browser_last_error - see browser.h
 */
const char *browser_last_error(BrowserSession *s)
{
    if (!s) return NULL;
    return s->last_error;
}

#ifdef BROWSER_TEST
BrowserSession *browser_test_attach(CdpClient *c)
{
    BrowserSession *s = calloc(1, sizeof(BrowserSession));
    if (!s) return NULL;
    s->client = c;
    s->binary = str_dup("fake");
    s->timeout_ms = DEFAULT_TIMEOUT_MS;
    s->external_client = 1;
    return s;
}

void browser_test_set_strdup_fail(int nth_allocation)
{
    bstrdup_call_count = 0;
    bstrdup_fail_at = nth_allocation;
}

unsigned char *browser_test_decode_base64(const char *in, size_t *out_len)
{
    return decode_base64(in, out_len);
}

/**
 * browser_test_remove_profile_dir - run the profile removal with its
 * retry loop (test-only)
 * @dir: directory to remove.
 *
 * Regression hook for the "Directory not empty" close race: the real
 * retry behavior, callable without a browser session.
 */
void browser_test_remove_profile_dir(const char *dir)
{
    remove_profile_dir(dir);
}
#endif
