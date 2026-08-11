#define _GNU_SOURCE
#include <check.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "server/server.h"
#include "server/routes/routes.h"
#include "server/websocket.h"
#include "utils/logging.h"

/* test_server - unit tests for server. Depends on: check, the module under test. */
int server_test_parse_chunks(const char **chunks, const size_t *lengths, int count,
                             char *method, size_t method_size,
                             char *path, size_t path_size,
                             char *body, size_t body_size);
int server_test_static_path_safe(const char *path);
int server_test_open_static_file_beneath(const char *root, const char *path);
Client *server_test_client_new(void);
void server_test_client_free(Client *client);
void server_test_free_reset(void);
int server_test_free_tally(void);

/* uv capture stubs (selected by the SERVER_TEST macro in server.c): a real
 * libuv loop is unavailable here, so uv_write records the pending write for
 * the test to complete (or fail) by hand, and uv_is_closing reports that the
 * fake client handle is already closing so client_close frees it inline. */
static uv_write_t *captured_req = NULL;
static uv_write_cb captured_cb = NULL;
static const char *captured_buf = NULL;
static int stub_uv_write_fail = 0;

int server_test_uv_write(uv_write_t *req, uv_stream_t *stream,
                         const uv_buf_t bufs[], unsigned int nbufs,
                         uv_write_cb cb)
{
    (void)stream;
    (void)nbufs;
    if (stub_uv_write_fail) return -1;
    captured_req = req;
    captured_cb = cb;
    captured_buf = bufs[0].base;
    return 0;
}

int server_test_uv_is_closing(const uv_handle_t *handle)
{
    (void)handle;
    return 1;
}

const Route routes[] = {{0}};
const int routes_count = 0;

int route_match(const char *method, const char *path, const Route *route)
{
    (void)method;
    (void)path;
    (void)route;
    return 0;
}

int ws_do_handshake(HTTPRequest *req, Client *client, ServerContext *ctx)
{
    (void)req;
    (void)client;
    (void)ctx;
    return -1;
}

int middleware_check_unlock(HTTPRequest *req, ServerContext *ctx)
{
    (void)req;
    (void)ctx;
    return 0;
}

int middleware_check_unlock_query(HTTPRequest *req, ServerContext *ctx)
{
    (void)req;
    (void)ctx;
    return 0;
}

int middleware_has_valid_ws_token(const char *headers, const char *token)
{
    (void)headers;
    (void)token;
    return 0;
}

int rate_limiter_allow(RateLimiter *limiter, const char *key)
{
    (void)limiter;
    (void)key;
    return 1;
}

void log_msg(LogLevel level, const char *file, int line, const char *message, ...)
{
    (void)level;
    (void)file;
    (void)line;
    (void)message;
}

START_TEST(test_http_parser_accepts_fragmented_nonterminated_request)
{
    const char chunk1[] = {'P','O','S','T',' ','/','a','p','i','/','c','h','a','t',' ',
                           'H','T','T','P','/','1','.','1','\r','\n','C','o','n','t','e','n','t','-'};
    const char chunk2[] = {'L','e','n','g','t','h',':',' ','5','\r','\n','\r','\n','h','e'};
    const char chunk3[] = {'l','l','o'};
    const char *chunks[] = {chunk1, chunk2, chunk3};
    const size_t lengths[] = {sizeof(chunk1), sizeof(chunk2), sizeof(chunk3)};
    char method[16] = {0};
    char path[64] = {0};
    char body[16] = {0};
    ck_assert_int_eq(server_test_parse_chunks(chunks, lengths, 3,
                                              method, sizeof(method),
                                              path, sizeof(path),
                                              body, sizeof(body)), 1);
    ck_assert_str_eq(method, "POST");
    ck_assert_str_eq(path, "/api/chat");
    ck_assert_str_eq(body, "hello");
}
END_TEST

START_TEST(test_http_parser_rejects_malformed_content_length)
{
    const char request[] =
        "POST /api/chat HTTP/1.1\r\nContent-Length: -1\r\n\r\n";
    const char *chunks[] = {request};
    const size_t lengths[] = {sizeof(request) - 1};
    char method[16] = {0};
    char path[64] = {0};
    char body[16] = {0};
    ck_assert_int_eq(server_test_parse_chunks(chunks, lengths, 1,
                                              method, sizeof(method),
                                              path, sizeof(path),
                                              body, sizeof(body)), -1);
}
END_TEST

START_TEST(test_static_path_rejects_traversal)
{
    ck_assert_int_eq(server_test_static_path_safe("/assets/app.js"), 1);
    ck_assert_int_eq(server_test_static_path_safe("/../../etc/passwd"), 0);
    ck_assert_int_eq(server_test_static_path_safe("/assets/../secret"), 0);
    ck_assert_int_eq(server_test_static_path_safe("assets/app.js"), 0);
}
END_TEST

static char static_root[64];
static char static_outside[64];
static char static_outside_file[512];
static char static_link_path[512];

static void static_fs_setup(void)
{
    snprintf(static_root, sizeof(static_root), "/tmp/echo_static_root_XXXXXX");
    snprintf(static_outside, sizeof(static_outside), "/tmp/echo_static_out_XXXXXX");
    ck_assert_ptr_nonnull(mkdtemp(static_root));
    ck_assert_ptr_nonnull(mkdtemp(static_outside));

    ck_assert_int_lt(snprintf(static_outside_file, sizeof(static_outside_file),
                              "%s/secret", static_outside),
                     (int)sizeof(static_outside_file));
    FILE *file = fopen(static_outside_file, "w");
    ck_assert_ptr_nonnull(file);
    ck_assert_int_eq(fclose(file), 0);
    ck_assert_int_lt(snprintf(static_link_path, sizeof(static_link_path),
                              "%s/link", static_root),
                     (int)sizeof(static_link_path));
    ck_assert_int_eq(symlink(static_outside, static_link_path), 0);
}

static void static_fs_teardown(void)
{
    ck_assert_int_eq(unlink(static_link_path), 0);
    ck_assert_int_eq(unlink(static_outside_file), 0);
    ck_assert_int_eq(rmdir(static_outside), 0);
    ck_assert_int_eq(rmdir(static_root), 0);
}

START_TEST(test_static_open_rejects_symlink_escape)
{
    char escaped[512];
    ck_assert_int_lt(snprintf(escaped, sizeof(escaped), "%s/link/secret", static_root),
                     (int)sizeof(escaped));
    ck_assert_int_eq(server_test_open_static_file_beneath(static_root, escaped), -1);
}
END_TEST

/* L1 regression: server_response handed its asprintf'd buffer to uv_write
 * and write_done freed only the request — one leak per HTTP response. The
 * free tally below counts every free performed by server.c: on the old code
 * the response buffer was never freed (tally 5), on the fixed code the
 * write-completion packet (buf + wctx + req) is released (tally 7). */
START_TEST(test_server_response_frees_response_buffer_on_completion)
{
    server_test_free_reset();
    captured_req = NULL;
    captured_cb = NULL;
    captured_buf = NULL;
    stub_uv_write_fail = 0;
    Client *client = server_test_client_new();
    ck_assert_ptr_nonnull(client);
    ck_assert_int_eq(server_response(client, 200, "text/plain", "ok"), 0);
    ck_assert_ptr_nonnull(captured_req);
    ck_assert_ptr_nonnull(captured_buf);
    ck_assert_ptr_nonnull(strstr(captured_buf, "HTTP/1.1 200 OK"));
    /* Simulate libuv completing the write: the completion path must free
     * buf + wctx + req (3), then client_close_cb frees the client and its
     * three NULL fields (4). */
    captured_cb(captured_req, 0);
    ck_assert_int_eq(server_test_free_tally(), 7);
}
END_TEST

/* L1 regression, synchronous-failure side: uv_write failing must not leak
 * the buffer + request, and the connection must be closed (old code
 * ignored the return value and returned 0, leaking both). */
START_TEST(test_server_response_cleans_up_on_uv_write_failure)
{
    server_test_free_reset();
    stub_uv_write_fail = 1;
    Client *client = server_test_client_new();
    ck_assert_ptr_nonnull(client);
    ck_assert_int_eq(server_response(client, 200, "text/plain", "ok"), -1);
    ck_assert_int_eq(server_test_free_tally(), 7);
}
END_TEST

START_TEST(test_sse_write_frees_frame_on_completion)
{
    server_test_free_reset();
    captured_req = NULL;
    captured_cb = NULL;
    captured_buf = NULL;
    stub_uv_write_fail = 0;
    Client *client = server_test_client_new();
    ck_assert_ptr_nonnull(client);
    ck_assert_int_eq(server_sse_write(client, "data: hello\n\n"), 0);
    ck_assert_ptr_nonnull(captured_req);
    ck_assert_str_eq(captured_buf, "data: hello\n\n");
    captured_cb(captured_req, 0);
    /* sse_write_done frees buf + wctx + req; SSE never closes the client */
    ck_assert_int_eq(server_test_free_tally(), 3);
    server_test_client_free(client);
    ck_assert_int_eq(server_test_free_tally(), 7);
}
END_TEST

START_TEST(test_sse_write_cleans_up_on_uv_write_failure)
{
    server_test_free_reset();
    stub_uv_write_fail = 1;
    Client *client = server_test_client_new();
    ck_assert_ptr_nonnull(client);
    ck_assert_int_eq(server_sse_write(client, "data: hello\n\n"), -1);
    ck_assert_int_eq(server_test_free_tally(), 3);
    /* Failure path must not close an SSE client */
    server_test_client_free(client);
    ck_assert_int_eq(server_test_free_tally(), 7);
}
END_TEST

/* server.h documents "NULL is accepted" for server_stop; the old code
 * dereferenced ctx->loop unconditionally (crash on NULL). */
START_TEST(test_server_stop_null_ctx_is_noop)
{
    server_stop(NULL);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("Server");
    TCase *tc = tcase_create("Parsing");
    tcase_add_checked_fixture(tc, static_fs_setup, static_fs_teardown);
    tcase_add_test(tc, test_http_parser_accepts_fragmented_nonterminated_request);
    tcase_add_test(tc, test_http_parser_rejects_malformed_content_length);
    tcase_add_test(tc, test_static_path_rejects_traversal);
    tcase_add_test(tc, test_static_open_rejects_symlink_escape);
    tcase_add_test(tc, test_server_response_frees_response_buffer_on_completion);
    tcase_add_test(tc, test_server_response_cleans_up_on_uv_write_failure);
    tcase_add_test(tc, test_sse_write_frees_frame_on_completion);
    tcase_add_test(tc, test_sse_write_cleans_up_on_uv_write_failure);
    tcase_add_test(tc, test_server_stop_null_ctx_is_noop);
    tcase_set_timeout(tc, 30);
    suite_add_tcase(suite, tc);
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
