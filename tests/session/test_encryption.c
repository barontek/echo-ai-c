/*
 * test_encryption.c - file-store fault-injection: salt/pepper/verifier
 * creation must surface an fclose failure (a silently-lost flush) as -1
 * and leave no committed file, per AGENTS.md "Fault-injection testing".
 * Depends on: check, openssl.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "session/encryption.h"

extern void encryption_test_set_fclose_fail(int nth_close);

static char path[256];

static void setup_path(void)
{
    snprintf(path, sizeof(path), "/tmp/test_encryption_%d", getpid());
    unlink(path);
}

static void teardown_path(void)
{
    unlink(path);
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

/* Regression: encryption_salt_create returned 0 after an fclose failure on
 * the post-fwrite success path, silently losing the flushed salt. With the
 * fix it returns -1 and removes the half-written file. On the old code the
 * first assertion (rc == -1) fails. */
START_TEST(test_salt_create_fclose_fail_is_clean)
{
    encryption_test_set_fclose_fail(1);
    ck_assert_int_eq(encryption_salt_create(path), -1);
    encryption_test_set_fclose_fail(-1);
    ck_assert_int_eq(file_exists(path), 0);

    /* normal operation still works after the injected failure */
    ck_assert_int_eq(encryption_salt_create(path), 0);
    ck_assert_int_eq(file_exists(path), 1);
}
END_TEST

/* Same regression for the pepper store. */
START_TEST(test_pepper_create_fclose_fail_is_clean)
{
    encryption_test_set_fclose_fail(1);
    ck_assert_int_eq(encryption_pepper_create(path), -1);
    encryption_test_set_fclose_fail(-1);
    ck_assert_int_eq(file_exists(path), 0);

    ck_assert_int_eq(encryption_pepper_create(path), 0);
    ck_assert_int_eq(file_exists(path), 1);
}
END_TEST

/* Same regression for the verifier store (encryption_create_verifier). */
START_TEST(test_create_verifier_fclose_fail_is_clean)
{
    EncryptionKey key;
    memset(&key, 0, sizeof(key));

    encryption_test_set_fclose_fail(1);
    ck_assert_int_eq(encryption_create_verifier(&key, path), -1);
    encryption_test_set_fclose_fail(-1);

    ck_assert_int_eq(encryption_create_verifier(&key, path), 0);
    ck_assert_int_eq(file_exists(path), 1);
}
END_TEST

static Suite *encryption_suite(void)
{
    Suite *s = suite_create("encryption");
    TCase *tc = tcase_create("store_fault_injection");
    tcase_add_checked_fixture(tc, setup_path, teardown_path);
    tcase_add_test(tc, test_salt_create_fclose_fail_is_clean);
    tcase_add_test(tc, test_pepper_create_fclose_fail_is_clean);
    tcase_add_test(tc, test_create_verifier_fclose_fail_is_clean);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(encryption_suite());
    srunner_set_fork_status(sr, CK_FORK);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
