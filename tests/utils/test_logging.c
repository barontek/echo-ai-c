/*
 * test_logging.c - unit tests for the logging module: init status,
 * JSON output shape, and level filtering. Depends on: check, logging.
 */

#define _GNU_SOURCE
#include <check.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "utils/logging.h"

/* Redirect stderr to a temp file and return the fd to restore it
 * (dup'd beforehand). */
static int redirect_stderr_to(const char *path, int *saved_fd)
{
    *saved_fd = dup(STDERR_FILENO);
    ck_assert_int_ge(*saved_fd, 0);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ck_assert_int_ge(fd, 0);
    ck_assert_int_eq(dup2(fd, STDERR_FILENO), STDERR_FILENO);
    close(fd);
    return 0;
}

static char *read_capture(const char *path)
{
    FILE *f = fopen(path, "rb");
    ck_assert_ptr_nonnull(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)size + 1);
    ck_assert_ptr_nonnull(buf);
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

START_TEST(test_log_init_succeeds_with_healthy_stderr)
{
    ck_assert_int_eq(log_init(), 0);
}
END_TEST

/* A closed fd 2 (launcher misconfiguration) must surface as a -1 from
 * log_init, not as silent writes into the void. */
START_TEST(test_log_init_fails_when_stderr_closed)
{
    ck_assert_int_eq(close(STDERR_FILENO), 0);
    ck_assert_int_eq(log_init(), -1);
    /* restore a working stderr for the rest of the suite */
    int devnull = open("/dev/null", O_WRONLY);
    ck_assert_int_ge(devnull, 0);
    ck_assert_int_eq(dup2(devnull, STDERR_FILENO), STDERR_FILENO);
    close(devnull);
}
END_TEST

START_TEST(test_log_msg_emits_valid_json_record)
{
    char path[] = "/tmp/echo_log_capture_XXXXXX";
    int fd = mkstemp(path);
    ck_assert_int_ge(fd, 0);
    close(fd);
    int saved_fd;
    redirect_stderr_to(path, &saved_fd);

    log_set_level(LOG_DEBUG);
    log_msg(LOG_INFO, "test_file.c", 42, "hello log", "key", "value", NULL);
    fflush(stderr);

    ck_assert_int_eq(dup2(saved_fd, STDERR_FILENO), STDERR_FILENO);
    close(saved_fd);

    char *captured = read_capture(path);
    ck_assert(strstr(captured, "\"level\":\"info\"") != NULL);
    ck_assert(strstr(captured, "\"file\":\"test_file.c\"") != NULL);
    ck_assert(strstr(captured, "\"line\":42") != NULL);
    ck_assert(strstr(captured, "\"message\":\"hello log\"") != NULL);
    ck_assert(strstr(captured, "\"key\":\"value\"") != NULL);
    /* exactly one JSON record, no trailing garbage */
    ck_assert_int_eq(captured[strlen(captured) - 1], '\n');
    free(captured);
    ck_assert_int_eq(unlink(path), 0);
}
END_TEST

/* Below the current level nothing is written at all. */
START_TEST(test_log_msg_below_level_is_suppressed)
{
    char path[] = "/tmp/echo_log_filter_XXXXXX";
    int fd = mkstemp(path);
    ck_assert_int_ge(fd, 0);
    close(fd);
    int saved_fd;
    redirect_stderr_to(path, &saved_fd);

    log_set_level(LOG_ERROR);
    log_msg(LOG_INFO, "f.c", 1, "should not appear", NULL);
    fflush(stderr);

    ck_assert_int_eq(dup2(saved_fd, STDERR_FILENO), STDERR_FILENO);
    close(saved_fd);

    char *captured = read_capture(path);
    ck_assert_str_eq(captured, "");
    free(captured);
    ck_assert_int_eq(unlink(path), 0);
    log_set_level(LOG_WARN);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("Logging");
    TCase *tc = tcase_create("Init");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_log_init_succeeds_with_healthy_stderr);
    tcase_add_test(tc, test_log_init_fails_when_stderr_closed);
    suite_add_tcase(suite, tc);

    TCase *tc_output = tcase_create("Output");
    tcase_set_timeout(tc_output, 30);
    tcase_add_test(tc_output, test_log_msg_emits_valid_json_record);
    tcase_add_test(tc_output, test_log_msg_below_level_is_suppressed);
    suite_add_tcase(suite, tc_output);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
