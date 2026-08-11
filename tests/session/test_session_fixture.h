/*
 * test_session_fixture.h - shared fixtures for the session store test
 * binaries (manager/crud/oauth/branch): per-test temp dir, file
 * helpers, and the branch-record helpers. Depends on: check, the
 * session store under SESSION_MANAGER_TEST.
 */

#ifndef ECHO_TEST_SESSION_FIXTURE_H
#define ECHO_TEST_SESSION_FIXTURE_H

#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session/session_manager.h"
#include "session/session_branch.h"
#include "session/session.h"
#include "session/encryption.h"
#include "session/memory.h"
#include "agent/message.h"
#include "utils/string_utils.h"

/* Per-test temp dir (created fresh by tmpdir_setup, removed by
 * tmpdir_teardown); tests create session managers inside it. */
extern char tmpdir[64];

void tmpdir_setup(void);
void tmpdir_teardown(void);

size_t read_test_file(const char *path, unsigned char *buffer,
                      size_t capacity);
void write_test_file(const char *path, const unsigned char *data,
                     size_t length);

#endif /* ECHO_TEST_SESSION_FIXTURE_H */
