/*
 * web_fetch.c - URL fetch tool: downloads a page via libcurl with a
 * socket-policy hook and a size cap, extracts text for the LLM, and
 * retries WAF challenge pages through a curl-impersonate binary when one
 * is on PATH. Depends on: tool.h, libcurl, safety, string_utils,
 * html_extract, logging.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <curl/curl.h>

#include "tool.h"
#include "../safety/safety.h"
#include "../utils/http_client.h"
#include "../utils/string_utils.h"
#include "../utils/html_extract.h"
#include "../utils/logging.h"

typedef struct {
    SafetyConfig *safety;
} WebCtx;

/* Per-call state for the fetch, stack-declared in web_fetch_execute so it
 * dies with the frame: no shared/static state that concurrent calls or a
 * stale reuse could corrupt. */
typedef struct {
    SafetyConfig *safety;
    int socket_policy_rejected;
} FetchCtx;

static curl_socket_t open_socket_cb(void *userdata, curlsocktype purpose,
                                    struct curl_sockaddr *address)
{
    (void)purpose;
    FetchCtx *fc = userdata;
    if (!safety_check_socket_address((const struct sockaddr *)&address->addr))
    {
        fc->socket_policy_rejected = 1;
        return CURL_SOCKET_BAD;
    }
    return socket(address->family, address->socktype, address->protocol);
}

/* ---- Optional JS-challenge fallback ---------------------------------
 * WAFs (Cloudflare, DataDome, simple JS gates) serve challenge pages to
 * non-browser clients even when the site allows automated access.  A
 * header-shaped libcurl request cannot pass these because they require a
 * real JS engine.  If a curl-impersonate binary happens to be on PATH
 * (nix dev shell, Homebrew, a server image), retry challenged pages
 * through it: it presents a full Chrome TLS fingerprint plus header set,
 * which is enough for header/TLS-level challenges (verified against
 * sncf-connect.com, turkishairlines.com, skyscanner.com).  This is an
 * optional capability, never a dependency: no binary -> the original
 * result is returned unchanged.  Sites whose challenge is genuine
 * JS-execution (e.g. DataDome on kiwi.com) still come back blocked. */

#define IMPERSONATE_TIMEOUT_S 30
/* Only the first chunk of the body is scanned: challenge pages are small
 * and this keeps binary blobs (which never match) cheap to check. */
#define IMPERSONATE_SCAN_LEN 65536

/* Case-insensitive scan for challenge-page markers. */
static int looks_like_challenge(const char *data, size_t len)
{
    static const char *markers[] = {
        "just a moment", "client challenge", "enable js",
        "enable javascript", "ad blocker", "cf-browser-verification",
        "challenge-platform", "attention required", "ddos protection",
        "verify you", "checking your browser", NULL
    };
    if (!data || len == 0) return 0;
    size_t scan = len < IMPERSONATE_SCAN_LEN ? len : IMPERSONATE_SCAN_LEN;
    char *lower = malloc(scan + 1);
    if (!lower) return 0;
    for (size_t i = 0; i < scan; i++)
        lower[i] = (char)((data[i] >= 'A' && data[i] <= 'Z') ? data[i] + 32 : data[i]);
    lower[scan] = '\0';
    int hit = 0;
    for (int i = 0; markers[i]; i++)
    {
        if (strstr(lower, markers[i])) {
            hit = 1;
            break;
        }
    }
    free(lower);
    return hit;
}

/* Returns 1 when an executable of the given name exists on PATH. */
static int binary_on_path(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || !name) return 0;
    const char *p = path;
    while (*p)
    {
        const char *end = strchr(p, ':');
        size_t dlen = end ? (size_t)(end - p) : strlen(p);
        if (dlen > 0)
        {
            char full[4096];
            int n = snprintf(full, sizeof(full), "%.*s/%s", (int)dlen, p, name);
            if (n > 0 && (size_t)n < sizeof(full) && access(full, X_OK) == 0)
                return 1;
        }
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

/* Executes the impersonator binary and captures its stdout.  Returns a
 * malloc'd body (caller frees) or NULL on failure/timeout/OOM/size-limit.
 * Protocol and redirect behavior mirror the libcurl path: http(s) only,
 * no redirect following (CURLOPT_FOLLOWLOCATION stays 0). */
static char *fetch_via_impersonator(const char *binary, const char *imp_flag,
                                    const char *url, size_t limit,
                                    int timeout_s, size_t *out_len)
{
    int out_pipe[2];
    if (pipe(out_pipe) != 0) return NULL;

    pid_t pid = fork();
    if (pid < 0)
    {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return NULL;
    }

    if (pid == 0)
    {
        close(out_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        for (int i = 3; i < 256; i++) close(i);
        char timeout_arg[32];
        snprintf(timeout_arg, sizeof(timeout_arg), "%d", timeout_s);
        /* --compressed: the impersonated Chrome header set advertises
         * Accept-Encoding, so the server may send gzip/br; curl must be
         * told to decode it or the extractor sees compressed bytes. */
        if (imp_flag)
            execlp(binary, binary, "-s", "--compressed", "--max-time", timeout_arg,
                   "--noproxy", "*", "--proto", "https,http",
                   "--impersonate", imp_flag, url, (char *)NULL);
        else
            execlp(binary, binary, "-s", "--compressed", "--max-time", timeout_arg,
                   "--noproxy", "*", "--proto", "https,http",
                   url, (char *)NULL);
        _exit(127);
    }

    close(out_pipe[1]);

    HttpBuffer buf = {.limit = limit};
    int deadline_ms = timeout_s > 0 ? timeout_s * 1000 : 30000;
    int elapsed_ms = 0;
    int status = 0;
    char chunk[8192];

    for (;;)
    {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;                 /* child exited, drain below */
        if (r < 0 && errno != EINTR) break;

        if (elapsed_ms >= deadline_ms)
        {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            free(buf.data);
            close(out_pipe[0]);
            return NULL;                     /* timed out */
        }

        struct pollfd pfd = {.fd = out_pipe[0], .events = POLLIN};
        int pr = poll(&pfd, 1, 100);
        if (pr < 0 && errno != EINTR)
        {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            free(buf.data);
            close(out_pipe[0]);
            return NULL;
        }
        if (pr == 0) {
            elapsed_ms += 100;
            continue;
        }
        ssize_t n = read(out_pipe[0], chunk, sizeof(chunk));
        if (n > 0)
        {
            if (http_buffer_append(&buf, chunk, (size_t)n) != 0)
            {
                kill(pid, SIGKILL);          /* body exceeded the cap */
                waitpid(pid, &status, 0);
                free(buf.data);
                close(out_pipe[0]);
                return NULL;
            }
        }
        elapsed_ms += 100;
    }

    for (;;)
    {
        ssize_t n = read(out_pipe[0], chunk, sizeof(chunk));
        if (n <= 0) break;
        if (http_buffer_append(&buf, chunk, (size_t)n) != 0)
        {
            free(buf.data);
            close(out_pipe[0]);
            return NULL;
        }
    }
    close(out_pipe[0]);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || buf.len == 0)
    {
        free(buf.data);
        return NULL;
    }
    *out_len = buf.len;
    return buf.data;
}

/* Retries the fetch through one impersonator binary, replacing the body
 * and content type on success.  With require_challenge set the body must
 * first look like a challenge page; without it (libcurl-level failure)
 * any non-empty body counts as a win.  Returns 1 when replaced. */
static int retry_with_impersonator_binary(HttpBuffer *buf, char **ctype,
                                          const char *url, const char *binary,
                                          const char *imp_flag,
                                          int require_challenge,
                                          int timeout_s)
{
    if (require_challenge && !looks_like_challenge(buf->data, buf->len))
        return 0;
    if (!binary_on_path(binary)) return 0;
    size_t ilen = 0;
    char *ibody = fetch_via_impersonator(binary, imp_flag, url, buf->limit,
                                         timeout_s, &ilen);
    if (!ibody) return 0;
    free(buf->data);
    buf->data = ibody;
    buf->len = ilen;
    buf->cap = ilen + 1;
    buf->too_large = 0;
    char *html = str_dup("text/html");
    if (html)
    {
        free(*ctype);
        *ctype = html;
    }
    return 1;
}

/* Tries every known impersonator binary name until one succeeds. */
static int retry_with_impersonator(HttpBuffer *buf, char **ctype, const char *url,
                                   int require_challenge, int timeout_s)
{
    static const struct
    {
        const char *name;
        const char *flag;
    } candidates[] = {
        {"curl-impersonate-chrome", NULL},  /* Homebrew / upstream */
        {"curl-impersonate", "chrome136"},  /* nixpkgs generic */
        {"curl_chrome136", NULL},           /* nixpkgs versioned */
        {NULL, NULL}
    };
    for (int i = 0; candidates[i].name; i++)
    {
        if (retry_with_impersonator_binary(buf, ctype, url, candidates[i].name,
                                           candidates[i].flag,
                                           require_challenge, timeout_s))
            return 1;
    }
    return 0;
}

/* Error-path gate: the impersonator subprocess does its own resolution and
 * connect with no socket-policy hook, so suppress the retry when the policy
 * rejected an address, or when resolution failed (open_socket_cb never ran,
 * so no address was ever validated; the subprocess' second lookup could
 * answer differently). Bot walls never present as resolve failures. */
static int error_path_retry_with_impersonator(HttpBuffer *buf, char **ctype,
                                              const char *url, CURLcode res,
                                              const FetchCtx *fc,
                                              int timeout_s)
{
    if (fc->socket_policy_rejected || res == CURLE_COULDNT_RESOLVE_HOST)
        return 0;
    return retry_with_impersonator(buf, ctype, url, 0, timeout_s);
}

/* Configures the curl handle for a web fetch: timeout, protocols,
 * browser-shaped headers, socket policy hook, and the response buffer.
 * Returns the handle (caller cleans up), or NULL on init failure. */
static CURL *web_fetch_setup_curl(WebCtx *ctx, const char *url, FetchCtx *fctx,
                                  HttpBuffer *buf, struct curl_slist **hdrs_out)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    /* url stays alive: it is re-used by the impersonator fallback below. */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    /* Bot-detection hygiene: HTTP/2 and a browser-shaped header set. A bare
     * UA (e.g. "EchoAI/1.0") plus HTTP/1.1 gets 403'd by Cloudflare/WAF
     * filters even on pages that allow automated access. */
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8");
    hdrs = curl_slist_append(hdrs, "Accept-Language: en-US,en;q=0.9");
    hdrs = curl_slist_append(hdrs, "Upgrade-Insecure-Requests: 1");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Dest: document");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Mode: navigate");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Site: none");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-User: ?1");
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    /* Accept-Encoding is negotiated by curl itself ("" = all supported
     * encodings); without this the server's gzip/br response arrives
     * undecoded and the extractor sees compressed bytes. */
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, open_socket_cb);
    fctx->safety = ctx->safety;
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, fctx);

    buf->limit = ctx->safety->max_file_size;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_buffer_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    *hdrs_out = hdrs;
    return curl;
}

/* Converts a still-failing fetch into the appropriate ToolResult:
 * network-policy rejection, oversize, or a generic curl error with
 * context. Consumes the buffer/ctype/url. */
static ToolResult *web_fetch_error_result(FetchCtx *fctx, HttpBuffer *buf,
                                          char *ctype_copy, char *url,
                                          CURLcode res)
{
    free(buf->data);
    free(ctype_copy);
    free(url);
    if (fctx->socket_policy_rejected)
        return tool_result_error("connection blocked by network policy",
                                 "policy_denied");
    if (buf->too_large)
        return tool_result_error("response too large", "policy_denied");
    char *err = NULL;
    if (asprintf(&err, "HTTP request failed: %s", curl_easy_strerror(res)) < 0)
        err = str_dup("HTTP request failed");
    ToolResult *tr = tool_result_error(err, "execution_error");
    free(err);
    return tr;
}

static ToolResult *web_fetch_execute(Tool *self, const char *args_json)
{
    WebCtx *ctx = self->ctx;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return tool_result_error("invalid arguments JSON", "validation_error");

    cJSON *url_json = cJSON_GetObjectItem(args, "url");
    if (!url_json || !cJSON_IsString(url_json))
    {
        cJSON_Delete(args);
        return tool_result_error("missing 'url' argument", "validation_error");
    }

    char *url = str_dup(cJSON_GetStringValue(url_json));
    cJSON_Delete(args);

    if (!safety_check_url(ctx->safety, url))
    {
        free(url);
        return tool_result_error("URL rejected by network policy", "policy_denied");
    }

    FetchCtx fctx = {0};
    HttpBuffer buf = {0};
    struct curl_slist *hdrs = NULL;
    CURL *curl = web_fetch_setup_curl(ctx, url, &fctx, &buf, &hdrs);
    if (!curl)
    {
        free(url);
        return tool_result_error("curl init failed", "execution_error");
    }

    CURLcode res = curl_easy_perform(curl);

    /* Capture the Content-Type before cleanup: it decides whether the body
     * is extracted (HTML), truncated (text) or replaced by a descriptor. */
    char *ctype = NULL;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ctype);
    char *ctype_copy = ctype ? str_dup(ctype) : NULL;
    curl_easy_cleanup(curl);
    curl_slist_free_all(hdrs);

    if (res != CURLE_OK)
    {
        /* Connection-level bot block (TLS drop, RST, ...): before giving
         * up, let the impersonator try — it wins where the server rejects
         * the TLS fingerprint outright. */
        if (error_path_retry_with_impersonator(&buf, &ctype_copy, url, res,
                                               &fctx, IMPERSONATE_TIMEOUT_S))
            res = CURLE_OK;
    }
    else
    {
        /* Challenge page served with a 200/4xx body: retry only if the
         * body actually looks like one. */
        retry_with_impersonator(&buf, &ctype_copy, url, 1,
                                IMPERSONATE_TIMEOUT_S);
    }

    if (res != CURLE_OK)
        return web_fetch_error_result(&fctx, &buf, ctype_copy, url, res);
    free(url);

    char *simplified = content_extract_for_llm_alloc(ctype_copy, buf.data, buf.len,
                                               ctx->safety->web_fetch_max_chars);
    free(ctype_copy);
    if (!simplified)
    {
        free(buf.data);
        return tool_result_error("out of memory during content extraction",
                                 "execution_error");
    }

    ToolResult *tr = tool_result_create(simplified);
    free(simplified);
    free(buf.data);
    return tr;
}

static void web_fetch_destroy(Tool *self)
{
    if (!self) return;
    free(self->name);
    free(self->description);
    free(self->parameters_schema);
    free(self->ctx);
    free(self);
}

/**
 * tool_web_fetch_create - construct the web_fetch tool
 * @safety: borrowed SafetyConfig consulted on every execution; not owned
 *
 * Return: heap-allocated Tool, or NULL on OOM. Caller owns the Tool and
 * must release it with tool->destroy(); the safety pointer is borrowed,
 * never freed by the tool.
 */
Tool *tool_web_fetch_create(SafetyConfig *safety)
{
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return NULL;

    WebCtx *ctx = calloc(1, sizeof(WebCtx));
    if (!ctx) {
        free(t);
        return NULL;
    }
    ctx->safety = safety;

    t->name = str_dup("web_fetch");
    t->description = str_dup("Fetch a URL and return its extracted text "
                             "(HTML boilerplate stripped, links as citations)");
    t->parameters_schema = str_dup(
        "{\"type\":\"object\",\"properties\":{"
        "\"url\":{\"type\":\"string\",\"description\":\"URL to fetch\"}"
        "},\"required\":[\"url\"]}"
    );
    t->execute = web_fetch_execute;
    t->destroy = web_fetch_destroy;
    t->ctx = ctx;
    return t;
}

#ifdef WEB_FETCH_TEST
/* Test-only exports: keep the challenge/impersonator logic unit-testable
 * without any network or a real curl-impersonate install. */
#include <netinet/in.h>

/**
 * web_fetch_test_looks_like_challenge - scan a body for WAF challenge-page
 * markers
 * @data: body bytes; NULL-safe
 * @len: length of data
 *
 * Return: 1 when a challenge marker is found, 0 otherwise.
 */
int web_fetch_test_looks_like_challenge(const char *data, size_t len)
{
    return looks_like_challenge(data, len);
}

/**
 * web_fetch_test_binary_on_path - probe PATH for an executable
 * @name: executable name to look up; NULL-safe
 *
 * Return: 1 when an executable of that name exists on PATH, 0 otherwise.
 */
int web_fetch_test_binary_on_path(const char *name)
{
    return binary_on_path(name);
}

/**
 * web_fetch_test_retry_challenge - run the single-binary challenge retry
 * @data: in/out body buffer; on success *data is replaced (caller frees
 * the new buffer), on failure *data is untouched
 * @len: in/out body length
 * @limit: size cap enforced during the retry fetch
 * @ctype: in/out content-type string (replaced with "text/html" on
 * success); caller frees
 * @url: URL to refetch
 * @binary: impersonator binary to use
 * @imp_flag: curl-impersonate flag string, or NULL
 * @timeout_s: subprocess timeout in seconds
 *
 * Return: 1 when the body was replaced, 0 otherwise.
 */
int web_fetch_test_retry_challenge(char **data, size_t *len, size_t limit,
                                   char **ctype, const char *url,
                                   const char *binary, const char *imp_flag,
                                   int timeout_s)
{
    HttpBuffer buf = {.data = *data, .len = *len, .cap = *len + 1,
                    .limit = limit};
    int replaced = retry_with_impersonator_binary(&buf, ctype, url, binary,
                                                  imp_flag, 1, timeout_s);
    if (replaced)
    {
        *data = buf.data;
        *len = buf.len;
    }
    return replaced;
}

/**
 * web_fetch_test_error_path_retry - run the error-path impersonator gate
 * @data: in/out body buffer; on success *data is replaced (caller frees
 * the new buffer), on failure *data is untouched
 * @len: in/out body length
 * @limit: size cap enforced during the retry fetch
 * @ctype: in/out content-type string (replaced with "text/html" on
 * success); caller frees
 * @url: URL to refetch
 * @timeout_s: subprocess timeout in seconds
 * @policy_rejected: simulate the socket policy having rejected an address
 * (suppresses the retry)
 * @resolve_failed: simulate a resolve failure (maps to
 * CURLE_COULDNT_RESOLVE_HOST, which also suppresses the retry)
 *
 * Return: 1 when the body was replaced, 0 otherwise.
 */
int web_fetch_test_error_path_retry(char **data, size_t *len, size_t limit,
                                    char **ctype, const char *url,
                                    int timeout_s, int policy_rejected,
                                    int resolve_failed)
{
    HttpBuffer buf = {.data = *data, .len = *len, .cap = *len + 1,
                    .limit = limit};
    FetchCtx fc = {.socket_policy_rejected = policy_rejected};
    CURLcode res = resolve_failed ? CURLE_COULDNT_RESOLVE_HOST : CURLE_OK;
    int replaced = error_path_retry_with_impersonator(
        &buf, ctype, url, res, &fc, timeout_s);
    if (replaced)
    {
        *data = buf.data;
        *len = buf.len;
    }
    return replaced;
}

/**
 * web_fetch_test_open_socket_addr - run the real socket-policy callback
 * against a fabricated IPv4 address
 * @s_addr_be: address in network byte order
 * @out: receives the callback's socket, or CURL_SOCKET_BAD when rejected
 * (test must close a successful socket)
 *
 * Return: 1 when the socket policy rejected the address, 0 otherwise.
 */
int web_fetch_test_open_socket_addr(unsigned int s_addr_be,
                                    curl_socket_t *out)
{
    /* curl_sockaddr only carries 16 bytes of addr; fabricate the larger
     * structs via a union so the writes stay inside the declared storage. */
    union {
        struct curl_sockaddr ca;
        unsigned char storage[sizeof(struct curl_sockaddr) + 16];
    } u;
    memset(&u, 0, sizeof(u));
    u.ca.family = AF_INET;
    u.ca.socktype = SOCK_STREAM;
    u.ca.protocol = IPPROTO_TCP;
    u.ca.addrlen = sizeof(struct sockaddr_in);
    struct sockaddr_in *in = (struct sockaddr_in *)&u.ca.addr;
    in->sin_family = AF_INET;
    in->sin_addr.s_addr = s_addr_be;
    FetchCtx fc = {0};
    *out = open_socket_cb(&fc, CURLSOCKTYPE_IPCXN, &u.ca);
    return fc.socket_policy_rejected;
}

/**
 * web_fetch_test_open_socket_addr6 - run the real socket-policy callback
 * against a fabricated IPv6 address
 * @s6: 16-byte address in network byte order
 * @out: receives the callback's socket, or CURL_SOCKET_BAD when rejected
 * (test must close a successful socket)
 *
 * Return: 1 when the socket policy rejected the address, 0 otherwise.
 */
int web_fetch_test_open_socket_addr6(const unsigned char s6[16],
                                     curl_socket_t *out)
{
    union {
        struct curl_sockaddr ca;
        unsigned char storage[sizeof(struct curl_sockaddr) + 16];
    } u;
    memset(&u, 0, sizeof(u));
    u.ca.family = AF_INET6;
    u.ca.socktype = SOCK_STREAM;
    u.ca.protocol = IPPROTO_TCP;
    u.ca.addrlen = sizeof(struct sockaddr_in6);
    struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&u.ca.addr;
    in6->sin6_family = AF_INET6;
    memcpy(&in6->sin6_addr.s6_addr, s6, 16);
    FetchCtx fc = {0};
    *out = open_socket_cb(&fc, CURLSOCKTYPE_IPCXN, &u.ca);
    return fc.socket_policy_rejected;
}
#endif
