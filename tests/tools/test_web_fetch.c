#define _GNU_SOURCE
#include <check.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>

/* test_web_fetch - web_fetch tool unit tests. Depends on: check, the module under test. */
/* Exported by web_fetch.c when built with WEB_FETCH_TEST=1. */
extern int web_fetch_test_looks_like_challenge(const char *data, size_t len);
extern int web_fetch_test_binary_on_path(const char *name);
extern int web_fetch_test_retry_challenge(char **data, size_t *len,
                                          size_t max_len, char **ctype,
                                          const char *url, const char *binary,
                                          const char *imp_flag, int timeout_s);
extern int web_fetch_test_error_path_retry(char **data, size_t *len,
                                           size_t max_len, char **ctype,
                                           const char *url, int timeout_s,
                                           int policy_rejected,
                                           int resolve_failed);
extern int web_fetch_test_open_socket_addr(unsigned int s_addr_be,
                                           curl_socket_t *out);
extern int web_fetch_test_open_socket_addr6(const unsigned char s6[16],
                                            curl_socket_t *out);

static char test_dir[64];
static char *saved_path = NULL;

static void setup(void)
{
    snprintf(test_dir, sizeof(test_dir), "/tmp/echo_wf_test_XXXXXX");
    if (!mkdtemp(test_dir)) abort();
    saved_path = strdup(getenv("PATH") ? getenv("PATH") : "");
}

static void teardown(void)
{
    if (test_dir[0])
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/curl-impersonate-chrome", test_dir);
        unlink(path);
        snprintf(path, sizeof(path), "%s/spawn_marker", test_dir);
        unlink(path);
        rmdir(test_dir);
        test_dir[0] = '\0';
    }
    if (saved_path)
    {
        setenv("PATH", saved_path, 1);
        free(saved_path);
        saved_path = NULL;
    }
}

/* Writes an executable fake "curl-impersonate-chrome" into the fixture dir
 * whose stdout is exactly `body` (or which fails when body is NULL). */
static void write_fake_binary(const char *body)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/curl-impersonate-chrome", test_dir);
    FILE *f = fopen(path, "w");
    if (!f) abort();
    if (body)
        fprintf(f, "#!/bin/sh\nprintf '%%s' '%s'\n", body);
    else
        fprintf(f, "#!/bin/sh\nexit 1\n");
    fclose(f);
    chmod(path, 0755);
}

static void set_path_to_test_dir(void)
{
    /* saved_path can be long (nix dev shell): size dynamically so the
     * fake binary's own external commands (touch etc.) stay findable. */
    size_t need = strlen(test_dir) + 1 + strlen(saved_path) + 1;
    char *env = malloc(need);
    if (!env) abort();
    snprintf(env, need, "%s:%s", test_dir, saved_path);
    setenv("PATH", env, 1);
    free(env);
}

static void set_path_to_empty(void)
{
    setenv("PATH", test_dir, 1);
}

/* Writes a fake impersonator that emits `body` on stdout AND touches
 * spawn_marker in the fixture dir, proving the binary was executed. */
static void write_marker_fake_binary(const char *body)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/curl-impersonate-chrome", test_dir);
    FILE *f = fopen(path, "w");
    if (!f) abort();
    fprintf(f, "#!/bin/sh\ntouch %s/spawn_marker\nprintf '%%s' '%s'\n",
            test_dir, body);
    fclose(f);
    chmod(path, 0755);
}

static int marker_exists(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/spawn_marker", test_dir);
    return access(path, F_OK) == 0;
}

/* --- challenge detection ------------------------------------------- */

START_TEST(test_detects_cloudflare_interstitial)
{
    const char *page = "<html><head><title>Just a moment...</title></head>"
                       "<body>Checking your browser before accessing.</body></html>";
    ck_assert_int_eq(web_fetch_test_looks_like_challenge(page, strlen(page)), 1);
}
END_TEST

START_TEST(test_detects_datadome_and_js_gates)
{
    const char *a = "Client Challenge - enable JavaScript";
    const char *b = "Please enable JS and disable any ad blocker";
    ck_assert_int_eq(web_fetch_test_looks_like_challenge(a, strlen(a)), 1);
    ck_assert_int_eq(web_fetch_test_looks_like_challenge(b, strlen(b)), 1);
}
END_TEST

START_TEST(test_detection_is_case_insensitive)
{
    const char *a = "Title: CLIENT CHALLENGE";
    const char *b = "Title: JUST A MOMENT...";
    ck_assert_int_eq(web_fetch_test_looks_like_challenge(a, strlen(a)), 1);
    ck_assert_int_eq(web_fetch_test_looks_like_challenge(b, strlen(b)), 1);
}
END_TEST

START_TEST(test_ignores_normal_pages_and_empty_input)
{
    const char *page = "<html><head><title>Istanbul to Lille</title></head>"
                       "<body><p>train schedules and fares</p></body></html>";
    ck_assert_int_eq(web_fetch_test_looks_like_challenge(page, strlen(page)), 0);
    ck_assert_int_eq(web_fetch_test_looks_like_challenge(NULL, 10), 0);
    ck_assert_int_eq(web_fetch_test_looks_like_challenge(page, 0), 0);
}
END_TEST

/* --- PATH lookup ---------------------------------------------------- */

START_TEST(test_binary_on_path_finds_and_misses)
{
    set_path_to_empty();
    ck_assert_int_eq(web_fetch_test_binary_on_path("curl-impersonate-chrome"), 0);
    write_fake_binary("irrelevant");
    set_path_to_test_dir();
    ck_assert_int_eq(web_fetch_test_binary_on_path("curl-impersonate-chrome"), 1);
    ck_assert_int_eq(web_fetch_test_binary_on_path("does-not-exist-12345"), 0);
}
END_TEST

/* --- retry through a fake impersonator ------------------------------ */

START_TEST(test_retry_replaces_challenge_body)
{
    write_fake_binary("<html><head><title>Real Page</title></head>"
                      "<body><p>real content text here for the test</p></body></html>");
    set_path_to_test_dir();

    char *body = strdup("Title: Just a moment...\n");
    char *ctype = strdup("text/html");
    size_t len = strlen(body);
    int replaced = web_fetch_test_retry_challenge(
        &body, &len, 100000, &ctype,
        "https://www.example.com/route", "curl-impersonate-chrome", NULL, 10);
    ck_assert_int_eq(replaced, 1);
    ck_assert_int_gt(len, 0);
    ck_assert(strstr(body, "real content text here") != NULL);
    ck_assert(strstr(body, "Just a moment") == NULL);
    ck_assert_str_eq(ctype, "text/html");
    free(body);
    free(ctype);
}
END_TEST

START_TEST(test_retry_leaves_normal_page_alone)
{
    write_fake_binary("<html><body>should not be used</body></html>");
    set_path_to_test_dir();

    char *body = strdup("<html><head><title>Istanbul to Lille</title></head>"
                        "<body><p>train schedules</p></body></html>");
    char *ctype = strdup("text/html");
    char *orig = body;
    size_t len = strlen(body);
    int replaced = web_fetch_test_retry_challenge(
        &body, &len, 100000, &ctype,
        "https://www.example.com/route", "curl-impersonate-chrome", NULL, 10);
    ck_assert_int_eq(replaced, 0);
    ck_assert_ptr_eq(body, orig);
    free(body);
    free(ctype);
}
END_TEST

START_TEST(test_retry_without_binary_keeps_body)
{
    set_path_to_empty();

    char *body = strdup("Title: Just a moment...\n");
    char *ctype = strdup("text/html");
    char *orig = body;
    size_t len = strlen(body);
    int replaced = web_fetch_test_retry_challenge(
        &body, &len, 100000, &ctype,
        "https://www.example.com/route", "curl-impersonate-chrome", NULL, 10);
    ck_assert_int_eq(replaced, 0);
    ck_assert_ptr_eq(body, orig);
    free(body);
    free(ctype);
}
END_TEST

START_TEST(test_retry_binary_failure_keeps_body)
{
    write_fake_binary(NULL); /* script exits 1 */
    set_path_to_test_dir();

    char *body = strdup("Title: Just a moment...\n");
    char *ctype = strdup("text/html");
    char *orig = body;
    size_t len = strlen(body);
    int replaced = web_fetch_test_retry_challenge(
        &body, &len, 100000, &ctype,
        "https://www.example.com/route", "curl-impersonate-chrome", NULL, 10);
    ck_assert_int_eq(replaced, 0);
    ck_assert_ptr_eq(body, orig);
    free(body);
    free(ctype);
}
END_TEST

START_TEST(test_retry_times_out_keeps_body)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/curl-impersonate-chrome", test_dir);
    FILE *f = fopen(path, "w");
    if (!f) abort();
    fprintf(f, "#!/bin/sh\nsleep 5\n");
    fclose(f);
    chmod(path, 0755);
    set_path_to_test_dir();

    char *body = strdup("Title: Just a moment...\n");
    char *ctype = strdup("text/html");
    char *orig = body;
    size_t len = strlen(body);
    int replaced = web_fetch_test_retry_challenge(
        &body, &len, 100000, &ctype,
        "https://www.example.com/route", "curl-impersonate-chrome", NULL, 1);
    ck_assert_int_eq(replaced, 0);
    ck_assert_ptr_eq(body, orig);
    free(body);
    free(ctype);
}
END_TEST

START_TEST(test_retry_early_eof_still_waits_real_deadline)
{
    /* Regression for the parallel-suite flake: a child that closes its
     * stdout (here via exec >/dev/null) puts the pipe at EOF while the
     * child is still running, so poll() returns POLLIN instantly. The old
     * flat "elapsed += 100" accounting then burned the whole 1s deadline
     * budget in microseconds and returned before the child was even
     * reaped. The deadline is wall-clock time: this retry must take ~1s,
     * not ~1ms — the 900ms bound has a 100x margin over the old behavior. */
    char path[256];
    snprintf(path, sizeof(path), "%s/curl-impersonate-chrome", test_dir);
    FILE *f = fopen(path, "w");
    if (!f) abort();
    fprintf(f, "#!/bin/sh\nexec >/dev/null\nsleep 5\n");
    fclose(f);
    chmod(path, 0755);
    set_path_to_test_dir();

    char *body = strdup("Title: Just a moment...\n");
    char *ctype = strdup("text/html");
    char *orig = body;
    size_t len = strlen(body);
    struct timespec t0, tn;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int replaced = web_fetch_test_retry_challenge(
        &body, &len, 100000, &ctype,
        "https://www.example.com/route", "curl-impersonate-chrome", NULL, 1);
    clock_gettime(CLOCK_MONOTONIC, &tn);
    long elapsed_ms = (tn.tv_sec - t0.tv_sec) * 1000L +
                      (tn.tv_nsec - t0.tv_nsec) / 1000000L;
    ck_assert_int_eq(replaced, 0);
    ck_assert_ptr_eq(body, orig);
    ck_assert_int_ge((int)elapsed_ms, 900);
    free(body);
    free(ctype);
}
END_TEST

START_TEST(test_retry_size_limit_keeps_body)
{
    /* 5000 bytes of output against a 100-byte cap must be refused. */
    char path[256];
    snprintf(path, sizeof(path), "%s/curl-impersonate-chrome", test_dir);
    FILE *f = fopen(path, "w");
    if (!f) abort();
    fprintf(f, "#!/bin/sh\nhead -c 5000 /dev/zero | tr '\\0' 'x'\n");
    fclose(f);
    chmod(path, 0755);
    set_path_to_test_dir();

    char *body = strdup("Title: Just a moment...\n");
    char *ctype = strdup("text/html");
    char *orig = body;
    size_t len = strlen(body);
    int replaced = web_fetch_test_retry_challenge(
        &body, &len, 100, &ctype,
        "https://www.example.com/route", "curl-impersonate-chrome", NULL, 10);
    ck_assert_int_eq(replaced, 0);
    ck_assert_ptr_eq(body, orig);
    free(body);
    free(ctype);
}
END_TEST

/* --- SSRF gate: socket-policy callback ------------------------------- */

START_TEST(test_socket_cb_flags_private_address)
{
    curl_socket_t sock = 0;
    int rejected = web_fetch_test_open_socket_addr(inet_addr("169.254.169.254"),
                                                   &sock);
    ck_assert_int_eq(rejected, 1);
    ck_assert_int_eq(sock, CURL_SOCKET_BAD);
}
END_TEST

START_TEST(test_socket_cb_accepts_public_address)
{
    curl_socket_t sock = 0;
    int rejected = web_fetch_test_open_socket_addr(inet_addr("8.8.8.8"), &sock);
    ck_assert_int_eq(rejected, 0);
    ck_assert_int_ne(sock, CURL_SOCKET_BAD);
    if (sock != CURL_SOCKET_BAD) close(sock);
}
END_TEST

START_TEST(test_socket_cb_flags_ipv6_linklocal)
{
    unsigned char s6[16];
    ck_assert_int_eq(inet_pton(AF_INET6, "fe80::1", s6), 1);
    curl_socket_t sock = 0;
    int rejected = web_fetch_test_open_socket_addr6(s6, &sock);
    ck_assert_int_eq(rejected, 1);
    ck_assert_int_eq(sock, CURL_SOCKET_BAD);
}
END_TEST

/* --- SSRF gate: error-path suppression ------------------------------- */

START_TEST(test_error_path_suppressed_on_policy_rejection)
{
    write_marker_fake_binary("retried body");
    set_path_to_test_dir();

    char *body = strdup("partial body");
    char *ctype = strdup("text/html");
    char *orig = body;
    size_t len = strlen(body);
    int replaced = web_fetch_test_error_path_retry(
        &body, &len, 100000, &ctype,
        "https://www.example.com/route", 10, 1, 0);
    ck_assert_int_eq(replaced, 0);
    ck_assert_ptr_eq(body, orig);
    ck_assert_int_eq(marker_exists(), 0);
    free(body);
    free(ctype);
}
END_TEST

START_TEST(test_error_path_suppressed_on_resolve_failure)
{
    write_marker_fake_binary("retried body");
    set_path_to_test_dir();

    char *body = strdup("partial body");
    char *ctype = strdup("text/html");
    char *orig = body;
    size_t len = strlen(body);
    int replaced = web_fetch_test_error_path_retry(
        &body, &len, 100000, &ctype,
        "https://www.example.com/route", 10, 0, 1);
    ck_assert_int_eq(replaced, 0);
    ck_assert_ptr_eq(body, orig);
    ck_assert_int_eq(marker_exists(), 0);
    free(body);
    free(ctype);
}
END_TEST

START_TEST(test_error_path_retries_when_not_rejected)
{
    write_marker_fake_binary("retried body");
    set_path_to_test_dir();

    char *body = strdup("partial body");
    char *ctype = strdup("text/html");
    size_t len = strlen(body);
    int replaced = web_fetch_test_error_path_retry(
        &body, &len, 100000, &ctype,
        "https://www.example.com/route", 10, 0, 0);
    ck_assert_int_eq(replaced, 1);
    ck_assert(strstr(body, "retried body") != NULL);
    ck_assert_int_eq(marker_exists(), 1);
    free(body);
    free(ctype);
}
END_TEST

static Suite *web_fetch_suite(void)
{
    Suite *s = suite_create("web_fetch");
    TCase *tc = tcase_create("challenge_fallback");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_set_timeout(tc, 30);

    tcase_add_test(tc, test_detects_cloudflare_interstitial);
    tcase_add_test(tc, test_detects_datadome_and_js_gates);
    tcase_add_test(tc, test_detection_is_case_insensitive);
    tcase_add_test(tc, test_ignores_normal_pages_and_empty_input);
    tcase_add_test(tc, test_binary_on_path_finds_and_misses);
    tcase_add_test(tc, test_retry_replaces_challenge_body);
    tcase_add_test(tc, test_retry_leaves_normal_page_alone);
    tcase_add_test(tc, test_retry_without_binary_keeps_body);
    tcase_add_test(tc, test_retry_binary_failure_keeps_body);
    tcase_add_test(tc, test_retry_times_out_keeps_body);
    tcase_add_test(tc, test_retry_early_eof_still_waits_real_deadline);
    tcase_add_test(tc, test_retry_size_limit_keeps_body);
    tcase_add_test(tc, test_socket_cb_flags_private_address);
    tcase_add_test(tc, test_socket_cb_accepts_public_address);
    tcase_add_test(tc, test_socket_cb_flags_ipv6_linklocal);
    tcase_add_test(tc, test_error_path_suppressed_on_policy_rejection);
    tcase_add_test(tc, test_error_path_suppressed_on_resolve_failure);
    tcase_add_test(tc, test_error_path_retries_when_not_rejected);

    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = web_fetch_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? 0 : 1;
}
