/*
 * test_session_fixture.c - shared fixtures for the session store
 * test binaries: per-test temp dir, branch cleanup, and file
 * helpers. Split from test_session_manager.c (2026-08 file-length
 * compliance). Depends on: check, the session store.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "test_session_fixture.h"

static void branch_rm(const char *tmpdir);

/* Per-test temp dir: created fresh before every test (checked fixture)
 * and removed after, so tests never depend on each other's leftovers. */
char tmpdir[64];

void tmpdir_setup(void)
{
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/test_sm_XXXXXX");
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
}

void tmpdir_teardown(void)
{
    branch_rm(tmpdir);
}

size_t read_test_file(const char *path, unsigned char *buffer,
                             size_t capacity)
{
    FILE *file = fopen(path, "rb");
    ck_assert_ptr_nonnull(file);
    size_t count = fread(buffer, 1, capacity, file);
    ck_assert_int_eq(ferror(file), 0);
    ck_assert_int_eq(fclose(file), 0);
    return count;
}

void write_test_file(const char *path, const unsigned char *data,
                            size_t length)
{
    FILE *file = fopen(path, "wb");
    ck_assert_ptr_nonnull(file);
    ck_assert_uint_eq(fwrite(data, 1, length, file), length);
    ck_assert_int_eq(fclose(file), 0);
}


static void branch_rm(const char *tmpdir)
{
    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int rc = system(rm);
    (void)rc;
}
