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

int server_test_parse_chunks(const char **chunks, const size_t *lengths, int count,
                             char *method, size_t method_size,
                             char *path, size_t path_size,
                             char *body, size_t body_size);
int server_test_static_path_safe(const char *path);
int server_test_open_static_file_beneath(const char *root, const char *path);

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

START_TEST(test_static_open_rejects_symlink_escape)
{
    char root[] = "/tmp/echo_static_root_XXXXXX";
    char outside[] = "/tmp/echo_static_out_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(root));
    ck_assert_ptr_nonnull(mkdtemp(outside));

    char outside_file[512];
    char link_path[512];
    ck_assert_int_lt(snprintf(outside_file, sizeof(outside_file), "%s/secret", outside),
                     (int)sizeof(outside_file));
    FILE *file = fopen(outside_file, "w");
    ck_assert_ptr_nonnull(file);
    ck_assert_int_eq(fclose(file), 0);
    ck_assert_int_lt(snprintf(link_path, sizeof(link_path), "%s/link", root),
                     (int)sizeof(link_path));
    ck_assert_int_eq(symlink(outside, link_path), 0);

    char escaped[512];
    ck_assert_int_lt(snprintf(escaped, sizeof(escaped), "%s/link/secret", root),
                     (int)sizeof(escaped));
    ck_assert_int_eq(server_test_open_static_file_beneath(root, escaped), -1);

    ck_assert_int_eq(unlink(link_path), 0);
    ck_assert_int_eq(unlink(outside_file), 0);
    ck_assert_int_eq(rmdir(outside), 0);
    ck_assert_int_eq(rmdir(root), 0);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("Server");
    TCase *tc = tcase_create("Parsing");
    tcase_add_test(tc, test_http_parser_accepts_fragmented_nonterminated_request);
    tcase_add_test(tc, test_http_parser_rejects_malformed_content_length);
    tcase_add_test(tc, test_static_path_rejects_traversal);
    tcase_add_test(tc, test_static_open_rejects_symlink_escape);
    suite_add_tcase(suite, tc);
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
