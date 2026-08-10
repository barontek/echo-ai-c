#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include "session/session_manager.h"
#include "session/session_branch.h"
#include "session/encryption.h"
#include "session/memory.h"
#include "utils/string_utils.h"

/* Declared in migration.h under SESSION_MANAGER_TEST; the test binary
 * compiles migration.c with that define but does not include the header. */
extern void migration_test_set_rename_fail(int nth_rename);

static size_t read_test_file(const char *path, unsigned char *buffer,
                             size_t capacity)
{
    FILE *file = fopen(path, "rb");
    ck_assert_ptr_nonnull(file);
    size_t count = fread(buffer, 1, capacity, file);
    ck_assert_int_eq(ferror(file), 0);
    ck_assert_int_eq(fclose(file), 0);
    return count;
}

static void write_test_file(const char *path, const unsigned char *data,
                            size_t length)
{
    FILE *file = fopen(path, "wb");
    ck_assert_ptr_nonnull(file);
    ck_assert_uint_eq(fwrite(data, 1, length, file), length);
    ck_assert_int_eq(fclose(file), 0);
}

START_TEST(test_provider_oauth_encrypted_roundtrip_and_delete)
{
    char tmpdir[] = "/tmp/test_sm_oauth_roundtrip_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    SessionManager *sm = session_manager_create(tmpdir, "oauth_password");
    ck_assert_ptr_nonnull(sm);
    const char *credentials =
        "{\"access_token\":\"secret-access\",\"refresh_token\":\"secret-refresh\"}";
    ck_assert_int_eq(session_manager_save_provider_oauth(
                         sm, "openai", credentials), 0);

    sqlite3_stmt *stmt = NULL;
    ck_assert_int_eq(sqlite3_prepare_v2(
                         sm->db,
                         "SELECT data_encrypted FROM provider_oauth "
                         "WHERE provider='openai'",
                         -1, &stmt, NULL), SQLITE_OK);
    ck_assert_int_eq(sqlite3_step(stmt), SQLITE_ROW);
    ck_assert_int_eq(sqlite3_column_type(stmt, 0), SQLITE_BLOB);
    const void *blob = sqlite3_column_blob(stmt, 0);
    int blob_len = sqlite3_column_bytes(stmt, 0);
    ck_assert_ptr_nonnull(blob);
    ck_assert_int_gt(blob_len, (int)strlen(credentials));
    ck_assert_int_ne(memcmp(blob, credentials, strlen(credentials)), 0);
    ck_assert_int_eq(sqlite3_step(stmt), SQLITE_DONE);
    ck_assert_int_eq(sqlite3_finalize(stmt), SQLITE_OK);

    char *loaded = NULL;
    ck_assert_int_eq(session_manager_load_provider_oauth_ex(
                         sm, "openai", &loaded), PROVIDER_OAUTH_LOAD_OK);
    ck_assert_ptr_nonnull(loaded);
    ck_assert_str_eq(loaded, credentials);
    free(loaded);
    ck_assert_int_eq(session_manager_delete_provider_oauth(sm, "openai"), 0);
    loaded = (char *)credentials;
    ck_assert_int_eq(session_manager_load_provider_oauth_ex(
                         sm, "openai", &loaded),
                     PROVIDER_OAUTH_LOAD_NOT_FOUND);
    ck_assert_ptr_null(loaded);

    session_manager_free(sm);
    char command[4096];
    ck_assert_int_lt(snprintf(command, sizeof(command), "rm -rf %s", tmpdir),
                     (int)sizeof(command));
    ck_assert_int_eq(system(command), 0);
}
END_TEST

START_TEST(test_legacy_post_commit_recovery_uses_encrypted_row_evidence)
{
    char tmpdir[] = "/tmp/test_sm_legacy_migrate_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    SessionManager *sm = session_manager_create(tmpdir, "old_password");
    ck_assert_ptr_nonnull(sm);
    Session *session = session_manager_create_session(sm, "legacy session");
    ck_assert_ptr_nonnull(session);
    char *session_id = str_dup(session->id);
    ck_assert_ptr_nonnull(session_id);
    session_free(session);

    char salt_path[4096];
    char old_salt_path[4096];
    char marker_path[4096];
    char verifier_path[4096];
    ck_assert_int_lt(snprintf(salt_path, sizeof(salt_path), "%s/salt", tmpdir),
                     (int)sizeof(salt_path));
    ck_assert_int_lt(snprintf(old_salt_path, sizeof(old_salt_path),
                              "%s/salt.old", tmpdir),
                     (int)sizeof(old_salt_path));
    ck_assert_int_lt(snprintf(marker_path, sizeof(marker_path),
                              "%s/.changing_pwd", tmpdir),
                     (int)sizeof(marker_path));
    ck_assert_int_lt(snprintf(verifier_path, sizeof(verifier_path),
                              "%s/.verifier", tmpdir),
                     (int)sizeof(verifier_path));
    unsigned char old_salt[64] = {0};
    unsigned char old_verifier[4096] = {0};
    size_t old_salt_len = read_test_file(salt_path, old_salt,
                                         sizeof(old_salt));
    size_t old_verifier_len = read_test_file(verifier_path, old_verifier,
                                             sizeof(old_verifier));
    ck_assert_uint_gt(old_salt_len, 0);
    ck_assert_uint_gt(old_verifier_len, 0);

    ck_assert_int_eq(migration_change_password(sm, "new_password"), 0);
    ck_assert_int_eq(sqlite3_exec(sm->db,
                                  "DROP TABLE password_migration_state",
                                  NULL, NULL, NULL), SQLITE_OK);
    write_test_file(old_salt_path, old_salt, old_salt_len);
    write_test_file(verifier_path, old_verifier, old_verifier_len);
    write_test_file(marker_path, (const unsigned char *)"pending\n", 8);
    session_manager_free(sm);

    ck_assert_ptr_null(session_manager_create(tmpdir, "wrong_password"));
    ck_assert_int_eq(access(marker_path, F_OK), 0);
    ck_assert_int_eq(access(old_salt_path, F_OK), 0);

    sm = session_manager_create(tmpdir, "new_password");
    ck_assert_ptr_nonnull(sm);
    ck_assert_int_eq(access(marker_path, F_OK), -1);
    ck_assert_int_eq(access(old_salt_path, F_OK), -1);
    session = session_manager_load_session(sm, session_id);
    ck_assert_ptr_nonnull(session);
    ck_assert_str_eq(session->title, "legacy session");
    session_free(session);
    free(session_id);
    session_manager_free(sm);

    char command[4096];
    ck_assert_int_lt(snprintf(command, sizeof(command), "rm -rf %s", tmpdir),
                     (int)sizeof(command));
    ck_assert_int_eq(system(command), 0);
}
END_TEST

START_TEST(test_provider_state_survives_encrypted_reload_and_retained_owner)
{
    char tmpdir[] = "/tmp/test_sm_provider_state_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    SessionManager *sm = session_manager_create(tmpdir, "state_password");
    ck_assert_ptr_nonnull(sm);
    SessionManager *retained = session_manager_retain(sm);
    ck_assert_ptr_eq(retained, sm);

    Session *session = session_manager_create_session(sm, "reasoning session");
    ck_assert_ptr_nonnull(session);
    session->messages = message_create("assistant", "tool requested");
    ck_assert_ptr_nonnull(session->messages);
    session->messages_count = 1;
    session->messages[0].provider_state = str_dup(
        "[{\"type\":\"reasoning\",\"encrypted_content\":\"cipher\"}]");
    session->messages[0].phase = str_dup("commentary");
    ck_assert_ptr_nonnull(session->messages[0].provider_state);
    ck_assert_ptr_nonnull(session->messages[0].phase);
    ck_assert_int_eq(session_manager_save_session(sm, session), 0);
    char *session_id = str_dup(session->id);
    ck_assert_ptr_nonnull(session_id);
    session_free(session);

    session_manager_free(sm);
    Session *loaded = session_manager_load_session(retained, session_id);
    ck_assert_ptr_nonnull(loaded);
    ck_assert_int_eq(loaded->messages_count, 1);
    ck_assert_str_eq(loaded->messages[0].provider_state,
        "[{\"type\":\"reasoning\",\"encrypted_content\":\"cipher\"}]");
    ck_assert_str_eq(loaded->messages[0].phase, "commentary");
    session_free(loaded);
    free(session_id);
    session_manager_free(retained);

    char command[4096];
    ck_assert_int_lt(snprintf(command, sizeof(command), "rm -rf %s", tmpdir),
                     (int)sizeof(command));
    ck_assert_int_eq(system(command), 0);
}
END_TEST

START_TEST(test_missing_verifier_never_accepts_or_commits_a_password)
{
    char tmpdir[] = "/tmp/test_sm_missing_verifier_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    SessionManager *sm = session_manager_create(tmpdir, "right_password");
    ck_assert_ptr_nonnull(sm);
    session_manager_free(sm);

    char verifier_path[4096];
    ck_assert_int_lt(snprintf(verifier_path, sizeof(verifier_path),
                              "%s/.verifier", tmpdir),
                     (int)sizeof(verifier_path));
    ck_assert_int_eq(unlink(verifier_path), 0);
    ck_assert_ptr_null(session_manager_create(tmpdir, "wrong_password"));
    ck_assert_int_eq(access(verifier_path, F_OK), -1);
    ck_assert_ptr_null(session_manager_create(tmpdir, "right_password"));
    ck_assert_int_eq(access(verifier_path, F_OK), -1);

    char command[4096];
    ck_assert_int_lt(snprintf(command, sizeof(command), "rm -rf %s", tmpdir),
                     (int)sizeof(command));
    ck_assert_int_eq(system(command), 0);
}
END_TEST

START_TEST(test_provider_oauth_typed_failures_preserve_credentials)
{
    char tmpdir[] = "/tmp/test_sm_oauth_failure_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    SessionManager *sm = session_manager_create(tmpdir, "right_password");
    ck_assert_ptr_nonnull(sm);
    const char *original = "{\"refresh_token\":\"keep-me\"}";
    ck_assert_int_eq(session_manager_save_provider_oauth(
                         sm, "openai", original), 0);

    session_manager_test_set_encrypt_fail(1);
    ck_assert_int_eq(session_manager_save_provider_oauth(
                         sm, "openai", "{\"refresh_token\":\"replace\"}"), -1);
    session_manager_test_set_encrypt_fail(-1);
    ck_assert_int_eq(sqlite3_exec(
                         sm->db,
                         "CREATE TRIGGER fail_oauth_update BEFORE UPDATE ON "
                         "provider_oauth BEGIN SELECT RAISE(ABORT, 'forced'); END",
                         NULL, NULL, NULL), SQLITE_OK);
    ck_assert_int_eq(session_manager_save_provider_oauth(
                         sm, "openai", "{\"refresh_token\":\"replace\"}"), -1);
    ck_assert_int_eq(sqlite3_exec(sm->db, "DROP TRIGGER fail_oauth_update",
                                  NULL, NULL, NULL), SQLITE_OK);

    char *loaded = NULL;
    ck_assert_int_eq(session_manager_load_provider_oauth_ex(
                         sm, "openai", &loaded), PROVIDER_OAUTH_LOAD_OK);
    ck_assert_str_eq(loaded, original);
    free(loaded);
    session_manager_test_set_oauth_alloc_fail(1);
    ck_assert_int_eq(session_manager_load_provider_oauth_ex(
                         sm, "openai", &loaded), PROVIDER_OAUTH_LOAD_OOM);
    ck_assert_ptr_null(loaded);
    session_manager_test_set_oauth_alloc_fail(-1);
    session_manager_test_set_bind_fail(1);
    ck_assert_int_eq(session_manager_load_provider_oauth_ex(
                         sm, "openai", &loaded), PROVIDER_OAUTH_LOAD_SQL_ERROR);
    ck_assert_ptr_null(loaded);
    session_manager_test_set_bind_fail(-1);
    ck_assert_int_eq(sqlite3_exec(
                         sm->db,
                         "UPDATE provider_oauth SET data_encrypted = X'00' "
                         "WHERE provider = 'openai'",
                         NULL, NULL, NULL), SQLITE_OK);
    ck_assert_int_eq(session_manager_load_provider_oauth_ex(
                         sm, "openai", &loaded),
                      PROVIDER_OAUTH_LOAD_DECRYPT_ERROR);
    ck_assert_ptr_null(loaded);
    session_manager_free(sm);

    char command[4096];
    ck_assert_int_lt(snprintf(command, sizeof(command), "rm -rf %s", tmpdir),
                     (int)sizeof(command));
    ck_assert_int_eq(system(command), 0);
}
END_TEST

START_TEST(test_password_change_migrates_oauth_and_recovers_post_commit)
{
    char tmpdir[] = "/tmp/test_sm_oauth_migrate_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    SessionManager *sm = session_manager_create(tmpdir, "old_password");
    ck_assert_ptr_nonnull(sm);
    const char *credentials = "{\"refresh_token\":\"survives-change\"}";
    ck_assert_int_eq(session_manager_save_provider_oauth(
                         sm, "openai", credentials), 0);
    Session *session = session_manager_create_session(sm, "surviving session");
    ck_assert_ptr_nonnull(session);
    char *session_id = str_dup(session->id);
    ck_assert_ptr_nonnull(session_id);
    session_free(session);

    char salt_path[4096];
    char old_salt_path[4096];
    char marker_path[4096];
    ck_assert_int_lt(snprintf(salt_path, sizeof(salt_path), "%s/salt", tmpdir),
                     (int)sizeof(salt_path));
    ck_assert_int_lt(snprintf(old_salt_path, sizeof(old_salt_path),
                              "%s/salt.old", tmpdir),
                     (int)sizeof(old_salt_path));
    ck_assert_int_lt(snprintf(marker_path, sizeof(marker_path),
                              "%s/.changing_pwd", tmpdir),
                     (int)sizeof(marker_path));
    unsigned char old_salt[64] = {0};
    size_t old_salt_len = read_test_file(salt_path, old_salt,
                                         sizeof(old_salt));
    ck_assert_uint_gt(old_salt_len, 0);

    ck_assert_int_eq(migration_change_password(sm, "new_password"), 0);
    char *loaded = NULL;
    ck_assert_int_eq(session_manager_load_provider_oauth_ex(
                         sm, "openai", &loaded), PROVIDER_OAUTH_LOAD_OK);
    ck_assert_str_eq(loaded, credentials);
    free(loaded);
    session = session_manager_load_session(sm, session_id);
    ck_assert_ptr_nonnull(session);
    ck_assert_str_eq(session->title, "surviving session");
    session_free(session);

    /* Simulate a crash after COMMIT but before salt.old/marker cleanup. */
    write_test_file(old_salt_path, old_salt, old_salt_len);
    write_test_file(marker_path, (const unsigned char *)"pending\n", 8);
    session_manager_free(sm);

    sm = session_manager_create(tmpdir, "new_password");
    ck_assert_ptr_nonnull(sm);
    ck_assert_int_eq(access(marker_path, F_OK), -1);
    ck_assert_int_eq(access(old_salt_path, F_OK), -1);
    ck_assert_int_eq(session_manager_load_provider_oauth_ex(
                         sm, "openai", &loaded), PROVIDER_OAUTH_LOAD_OK);
    ck_assert_str_eq(loaded, credentials);
    free(loaded);
    session = session_manager_load_session(sm, session_id);
    ck_assert_ptr_nonnull(session);
    ck_assert_str_eq(session->title, "surviving session");
    session_free(session);
    free(session_id);
    session_manager_free(sm);

    char command[4096];
    ck_assert_int_lt(snprintf(command, sizeof(command), "rm -rf %s", tmpdir),
                     (int)sizeof(command));
    ck_assert_int_eq(system(command), 0);
}
END_TEST

START_TEST(test_password_change_retries_verifier_swap_in_process)
{
    char tmpdir[] = "/tmp/test_sm_retry_swap_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    SessionManager *sm = session_manager_create(tmpdir, "old_password");
    ck_assert_ptr_nonnull(sm);
    const char *credentials = "{\"refresh_token\":\"still-valid\"}";
    ck_assert_int_eq(session_manager_save_provider_oauth(
                         sm, "openai", credentials), 0);
    Session *session = session_manager_create_session(sm, "swap session");
    ck_assert_ptr_nonnull(session);
    char *session_id = str_dup(session->id);
    ck_assert_ptr_nonnull(session_id);
    session_free(session);

    /* Renames inside migration_change_password: #1 salt -> salt.old,
     * #2 verifier.new -> verifier. Failing #2 simulates the post-commit
     * activation failure; the in-process recovery retry must complete
     * the swap and still return 0. */
    migration_test_set_rename_fail(2);
    ck_assert_int_eq(migration_change_password(sm, "new_password"), 0);
    migration_test_set_rename_fail(-1);

    char *loaded = NULL;
    ck_assert_int_eq(session_manager_load_provider_oauth_ex(
                         sm, "openai", &loaded), PROVIDER_OAUTH_LOAD_OK);
    ck_assert_str_eq(loaded, credentials);
    free(loaded);
    session = session_manager_load_session(sm, session_id);
    ck_assert_ptr_nonnull(session);
    ck_assert_str_eq(session->title, "swap session");
    session_free(session);
    free(session_id);
    session_manager_free(sm);

    /* The swap must have fully landed: the new password unlocks, the old
     * one is rejected, and no migration artifacts remain. */
    sm = session_manager_create(tmpdir, "new_password");
    ck_assert_ptr_nonnull(sm);
    session_manager_free(sm);
    ck_assert_ptr_null(session_manager_create(tmpdir, "old_password"));

    char marker_path[4096];
    char old_salt_path[4096];
    ck_assert_int_lt(snprintf(marker_path, sizeof(marker_path),
                              "%s/.changing_pwd", tmpdir),
                     (int)sizeof(marker_path));
    ck_assert_int_lt(snprintf(old_salt_path, sizeof(old_salt_path),
                              "%s/salt.old", tmpdir),
                     (int)sizeof(old_salt_path));
    ck_assert_int_eq(access(marker_path, F_OK), -1);
    ck_assert_int_eq(access(old_salt_path, F_OK), -1);

    char command[4096];
    ck_assert_int_lt(snprintf(command, sizeof(command), "rm -rf %s", tmpdir),
                     (int)sizeof(command));
    ck_assert_int_eq(system(command), 0);
}
END_TEST

START_TEST(test_password_change_rolls_back_on_malformed_oauth_row)
{
    char tmpdir[] = "/tmp/test_sm_oauth_bad_migration_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    SessionManager *sm = session_manager_create(tmpdir, "old_password");
    ck_assert_ptr_nonnull(sm);
    const char *credentials = "{\"refresh_token\":\"still-valid\"}";
    ck_assert_int_eq(session_manager_save_provider_oauth(
                         sm, "a-valid", credentials), 0);
    Session *session = session_manager_create_session(sm, "rollback session");
    ck_assert_ptr_nonnull(session);
    char *session_id = str_dup(session->id);
    ck_assert_ptr_nonnull(session_id);
    session_free(session);
    ck_assert_int_eq(sqlite3_exec(
                         sm->db,
                         "INSERT INTO provider_oauth(provider, data_encrypted) "
                         "VALUES('z-malformed', X'01')",
                         NULL, NULL, NULL), SQLITE_OK);

    ck_assert_int_eq(migration_change_password(sm, "new_password"), -1);
    char *loaded = NULL;
    ck_assert_int_eq(session_manager_load_provider_oauth_ex(
                         sm, "a-valid", &loaded), PROVIDER_OAUTH_LOAD_OK);
    ck_assert_str_eq(loaded, credentials);
    free(loaded);
    session = session_manager_load_session(sm, session_id);
    ck_assert_ptr_nonnull(session);
    ck_assert_str_eq(session->title, "rollback session");
    session_free(session);
    session_manager_free(sm);

    sm = session_manager_create(tmpdir, "old_password");
    ck_assert_ptr_nonnull(sm);
    ck_assert_int_eq(session_manager_load_provider_oauth_ex(
                         sm, "a-valid", &loaded), PROVIDER_OAUTH_LOAD_OK);
    ck_assert_str_eq(loaded, credentials);
    free(loaded);
    session = session_manager_load_session(sm, session_id);
    ck_assert_ptr_nonnull(session);
    ck_assert_str_eq(session->title, "rollback session");
    session_free(session);
    free(session_id);
    session_manager_free(sm);

    char command[4096];
    ck_assert_int_lt(snprintf(command, sizeof(command), "rm -rf %s", tmpdir),
                     (int)sizeof(command));
    ck_assert_int_eq(system(command), 0);
}
END_TEST

/* Regression test for A1: title is documented as encrypted but used to be
 * stored as plaintext in a `title TEXT` column. The fix renamed the column to
 * `title_encrypted BLOB` and encrypted/decrypted the title alongside the
 * messages/metadata/events blobs. This test asserts:
 *   1. The bytes persisted in the DB's title column are not the plaintext title.
 *   2. session_manager_load_session returns the original plaintext title.
 *   3. session_manager_list_sessions returns the original plaintext title.
 *
 * On the old (plaintext) code, assertion (1) fails because the stored bytes are
 * the literal title string. */
START_TEST(test_title_is_encrypted_at_rest)
{
    char tmpdir[] = "/tmp/test_sm_title_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));

    SessionManager *sm = session_manager_create(tmpdir, "test_password");
    ck_assert_ptr_nonnull(sm);

    const char *plain = "SecretTitle!@#";
    Session *s = session_manager_create_session(sm, plain);
    ck_assert_ptr_nonnull(s);
    ck_assert_str_eq(s->title, plain);
    session_free(s);

    /* (1) raw inspection of the DB: stored bytes must NOT equal the plaintext */
    char db_path[4096];
    snprintf(db_path, sizeof(db_path), "%s/echo-ai.db", tmpdir);
    sqlite3 *raw = NULL;
    ck_assert_int_eq(sqlite3_open(db_path, &raw), SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT title_encrypted FROM agent_sessions";
    ck_assert_int_eq(sqlite3_prepare_v2(raw, sql, -1, &stmt, NULL), SQLITE_OK);
    ck_assert_int_eq(sqlite3_step(stmt), SQLITE_ROW);
    const void *blob = sqlite3_column_blob(stmt, 0);
    int blob_len = sqlite3_column_bytes(stmt, 0);
    ck_assert_ptr_nonnull(blob);
    ck_assert_int_gt(blob_len, 0);
    /* Fernet token = 1 (version) + 8 (ts) + 16 (IV) + ciphertext + 32 (HMAC),
     * always strictly longer than the plaintext. On the old plaintext-code
     * path the stored bytes were exactly strlen(plain) long and equal to it. */
    ck_assert_int_gt(blob_len, (int)strlen(plain));
    ck_assert_int_ne(memcmp(blob, plain, strlen(plain)), 0);
    sqlite3_finalize(stmt);
    sqlite3_close(raw);

    /* (2) load_session round-trips the title */
    SessionList *list = session_manager_list_sessions(sm);
    ck_assert_ptr_nonnull(list);
    ck_assert_int_eq(list->count, 1);
    Session *loaded2 = session_manager_load_session(sm, list->ids[0]);
    session_list_free(list);
    ck_assert_ptr_nonnull(loaded2);
    ck_assert_str_eq(loaded2->title, plain);
    session_free(loaded2);

    /* (3) list_sessions also decrypts */
    SessionList *list2 = session_manager_list_sessions(sm);
    ck_assert_ptr_nonnull(list2);
    ck_assert_int_eq(list2->count, 1);
    ck_assert_str_eq(list2->titles[0], plain);
    session_list_free(list2);

    session_manager_free(sm);

    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int rc = system(rm);
    (void)rc;
}
END_TEST

START_TEST(test_session_list_realloc_failures_are_safe)
{
    char tmpdir[] = "/tmp/test_sm_realloc_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    SessionManager *sm = session_manager_create(tmpdir, "test_password");
    ck_assert_ptr_nonnull(sm);

    for (int i = 0; i < 17; i++)
    {
        char title[32];
        ck_assert_int_lt(snprintf(title, sizeof(title), "session %d", i),
                         (int)sizeof(title));
        Session *session = session_manager_create_session(sm, title);
        ck_assert_ptr_nonnull(session);
        session_free(session);
    }

    for (int fail_at = 1; fail_at <= 4; fail_at++)
    {
        session_manager_test_set_realloc_fail(fail_at);
        SessionList *list = session_manager_list_sessions(sm);
        ck_assert_ptr_null(list);
    }

    session_manager_test_set_realloc_fail(-1);
    SessionList *list = session_manager_list_sessions(sm);
    ck_assert_ptr_nonnull(list);
    ck_assert_int_eq(list->count, 17);
    session_list_free(list);
    session_manager_free(sm);

    char rm[4096];
    ck_assert_int_lt(snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir),
                     (int)sizeof(rm));
    int cleanup_rc = system(rm);
    ck_assert_int_eq(cleanup_rc, 0);
}
END_TEST

/* Regression test for A7: user_memory table used to be created by ad-hoc
 * memory_table_init calls scattered across main.c and routes.c. build_system_prompt
 * (via memory_list_all) had no guarantee the table existed. After the fix,
 * session_manager_create itself calls memory_table_init, so any consumer of a
 * SessionManager can call memory_set/memory_list_all immediately. On the old
 * code, this test would fail at memory_list_all with a prepare error because
 * no memory_table_init was called between session_manager_create and
 * memory_list_all. */
START_TEST(test_user_memory_table_ready_after_sm_create)
{
    char tmpdir[] = "/tmp/test_sm_mem_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));

    SessionManager *sm = session_manager_create(tmpdir, "pw");
    ck_assert_ptr_nonnull(sm);

    ck_assert_int_eq(memory_set(sm->db, "fact1", "value1"), 0);
    ck_assert_int_eq(memory_set(sm->db, "fact2", "value2"), 0);

    int count = 0;
    MemoryFact *facts = memory_list_all(sm->db, &count);
    ck_assert_ptr_nonnull(facts);
    ck_assert_int_eq(count, 2);
    memory_facts_free(facts, count);

    session_manager_free(sm);

    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int rc = system(rm);
    (void)rc;
}
END_TEST

/* Regression test for B1: save_session used to ignore sqlite3_bind_*
 * return codes; an SQLITE_NOMEM/TOOBIG would only surface later as a
 * generic "sqlite step save" error (or, in the worst case, write
 * partially-bound NULLs). After the fix, save_session checks every bind
 * and returns -1 if any bind fails. The test forces the Nth bind to
 * return SQLITE_NOMEM via the SESSION_MANAGER_TEST fault-injection knob
 * and asserts (a) save_session returns -1 and (b) the row in the DB
 * reflects the previously-saved state (not an empty overwrite), proving
 * the bind error aborted the save instead of committing partial data. */
START_TEST(test_save_session_bind_failure_aborts)
{
    char tmpdir[] = "/tmp/test_sm_bind_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));

    SessionManager *sm = session_manager_create(tmpdir, "pw");
    ck_assert_ptr_nonnull(sm);

    Session *s = session_manager_create_session(sm, "title_one");
    ck_assert_ptr_nonnull(s);
    session_manager_test_set_bind_fail(-1);

    /* Write a known-good session state first. */
    SessionList *list = session_manager_list_sessions(sm);
    ck_assert_ptr_nonnull(list);
    ck_assert_int_eq(list->count, 1);
    ck_assert_str_eq(list->titles[0], "title_one");
    char *sid = str_dup(list->ids[0]);
    session_list_free(list);
    session_free(s);

    /* Attempt a save that hits a bind failure mid-flight. add_message
     * invokes the load-modify-save cycle, so the save_session inside will
     * call 7 binds; we fail the 3rd (title_generation_attempted bind). */
    session_manager_test_set_bind_fail(3);
    int rc = session_manager_add_message(sm, sid, "user", "hi", NULL, NULL);
    ck_assert_int_eq(rc, -1);

    /* Reset and confirm the original session state is intact (the aborted
     * save did not commit a half-bound row over the existing id). */
    session_manager_test_set_bind_fail(-1);
    list = session_manager_list_sessions(sm);
    ck_assert_ptr_nonnull(list);
    ck_assert_int_eq(list->count, 1);
    ck_assert_str_eq(list->titles[0], "title_one");
    session_list_free(list);
    free(sid);

    session_manager_free(sm);

    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int rc2 = system(rm);
    (void)rc2;
}
END_TEST

/* Regression test for B2: session_manager_purge_sessions used to ignore the
 * sqlite3_step return value and bind return value, then read sqlite3_changes
 * regardless. On a bind or step error (SQLITE_BUSY, SQLITE_NOMEM, malformed
 * row) it reported "0 deleted" instead of -1, masking the failure. After the
 * fix, any non-OK bind and any non-DONE step return -1 via the standard error
 * path. The test forces a bind failure on the purge path and asserts -1. */
START_TEST(test_purge_sessions_bind_failure_returns_error)
{
    char tmpdir[] = "/tmp/test_sm_purge_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));

    SessionManager *sm = session_manager_create(tmpdir, "pw");
    ck_assert_ptr_nonnull(sm);

    Session *s = session_manager_create_session(sm, "to_delete");
    ck_assert_ptr_nonnull(s);
    session_free(s);

    session_manager_test_set_bind_fail(1); /* the 1st bind inside purge_sessions */
    int rc = session_manager_purge_sessions(sm, 0);
    ck_assert_int_eq(rc, -1);

    /* Reset and confirm a real purge still works (sanity). */
    session_manager_test_set_bind_fail(-1);
    rc = session_manager_purge_sessions(sm, 36500); /* max allowed by B9 */
    ck_assert_int_ge(rc, 0);

    session_manager_free(sm);

    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int syst = system(rm);
    (void)syst;
}
END_TEST

/* Regression test for B3: session_manager_delete_session used to return 0 for
 * both "row matched and deleted" and "no row matched", because SQLITE_DONE is
 * returned in both cases. Routes then answered {"deleted":true} for non-
 * existent ids, never 404. After the fix, delete_session returns 1 if a row
 * was deleted, 0 if none matched, -1 on error; routes maps 0 -> 404. */
START_TEST(test_delete_session_distinguishes_missing)
{
    char tmpdir[] = "/tmp/test_sm_del_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));

    SessionManager *sm = session_manager_create(tmpdir, "pw");
    ck_assert_ptr_nonnull(sm);

    /* no session yet -> delete must report "no row" */
    ck_assert_int_eq(session_manager_delete_session(sm, "does_not_exist"), 0);

    Session *s = session_manager_create_session(sm, "real");
    ck_assert_ptr_nonnull(s);
    char *sid = str_dup(s->id);
    session_free(s);

    ck_assert_int_eq(session_manager_delete_session(sm, sid), 1);
    /* second delete of the same id -> now missing */
    ck_assert_int_eq(session_manager_delete_session(sm, sid), 0);

    free(sid);
    session_manager_free(sm);

    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int syst = system(rm);
    (void)syst;
}
END_TEST

/* Regression test for B9: purge_sessions used to do
 * (time_t)older_than_days * 86400 with no validation, so a negative or huge
 * value could overflow (UB for signed) and flip the cutoff (deleting
 * everything or nothing). After the fix, purge_sessions validates the input
 * and refuses anything outside [0, 36500] with -1. */
START_TEST(test_purge_sessions_rejects_bad_days)
{
    char tmpdir[] = "/tmp/test_sm_purge_days_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));

    SessionManager *sm = session_manager_create(tmpdir, "pw");
    ck_assert_ptr_nonnull(sm);

    Session *s = session_manager_create_session(sm, "keep");
    ck_assert_ptr_nonnull(s);
    session_free(s);

    ck_assert_int_eq(session_manager_purge_sessions(sm, -1), -1);
    ck_assert_int_eq(session_manager_purge_sessions(sm, 36501), -1);
    ck_assert_int_eq(session_manager_purge_sessions(sm, 36500), 0); /* none ancient, OK (returns changes=0) */

    /* the session survived all bad-input purges and the harmless big-day purge */
    SessionList *list = session_manager_list_sessions(sm);
    ck_assert_ptr_nonnull(list);
    ck_assert_int_eq(list->count, 1);
    session_list_free(list);

    session_manager_free(sm);

    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int syst = system(rm);
    (void)syst;
}
END_TEST

/* Regression test for B10: empty/NULL ids used to silently bind SQL NULL,
 * producing either no match (DELETE on non-existent) or a primary-key-
 * constraint failure surfaced as a generic "step save" error. After the fix,
 * save_session refuses a NULL or empty session->id, and load_session refuses
 * an empty id, both up-front. */
START_TEST(test_empty_or_null_id_refused)
{
    char tmpdir[] = "/tmp/test_sm_id_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));

    SessionManager *sm = session_manager_create(tmpdir, "pw");
    ck_assert_ptr_nonnull(sm);

    /* load_session with empty id -> NULL (and no SQLite call is made) */
    ck_assert_ptr_null(session_manager_load_session(sm, ""));

    /* delete_session with empty id -> 0 (no row matched); the existing
     * `!id` NULL guard plus my new empty-string guard means we never send
     * "WHERE id = ''" to SQLite. */
    ck_assert_int_eq(session_manager_delete_session(sm, ""), 0);

    /* save_session with a NULL id -> -1 (no SQLite call) */
    Session *s = session_manager_create_session(sm, "real");
    ck_assert_ptr_nonnull(s);
    char *real_id = str_dup(s->id);
    free(s->id);
    s->id = NULL;
    ck_assert_int_eq(session_manager_save_session(sm, s), -1);

    /* restore the id and confirm save again works */
    s->id = str_dup(real_id);
    ck_assert_int_eq(session_manager_save_session(sm, s), 0);
    session_free(s);
    free(real_id);

    session_manager_free(sm);

    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int syst = system(rm);
    (void)syst;
}
END_TEST

/* Regression test for C4+C5: load_session used to silently mask decryption
 * failures (HMAC mismatch from wrong key, corrupted blob) by leaving the
 * Session's fields at their empty defaults and returning the Session. The
 * caller (agent_save_session) would then save the empties back over the
 * original blob, permanently destroying potentially-recoverable data. After
 * the fix, any decrypt or JSON-parse failure on a non-empty blob makes
 * load_session (a) log the specific failure and (b) return NULL so the
 * original row is preserved in the DB. */
START_TEST(test_load_returns_null_on_decrypt_failure_preserves_row)
{
    char tmpdir[] = "/tmp/test_sm_c45_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));

    /* First SM: real password, real session with real messages, saved. */
    SessionManager *sm = session_manager_create(tmpdir, "right_password");
    ck_assert_ptr_nonnull(sm);

    Session *s = session_manager_create_session(sm, "decrypt_test_title");
    ck_assert_ptr_nonnull(s);
    char *sid = str_dup(s->id);
    ck_assert_int_eq(session_manager_add_message(sm, sid, "user", "secret", NULL, NULL), 0);
    session_free(s);

    /* Confirm we can load it back under the right password. */
    s = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(s);
    ck_assert_int_eq(s->messages_count, 1);
    ck_assert_str_eq(s->messages[0].content, "secret");
    ck_assert_str_eq(s->title, "decrypt_test_title");
    session_free(s);
    session_manager_free(sm);

    /* Second SM: WRONG password. Same salt (already on disk) -> different
     * key -> HMAC mismatch on every blob. The new contract: load_session
     * returns NULL with informative logs, and the DB row is untouched. */
    sm = session_manager_create(tmpdir, "wrong_password");
    ck_assert_ptr_null(sm);

    /* Raw DB inspection: the messages blob is still there, still non-empty. */
    char db_path[4096];
    snprintf(db_path, sizeof(db_path), "%s/echo-ai.db", tmpdir);
    sqlite3 *raw = NULL;
    ck_assert_int_eq(sqlite3_open(db_path, &raw), SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT messages_encrypted FROM agent_sessions WHERE id = ?";
    ck_assert_int_eq(sqlite3_prepare_v2(raw, sql, -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
    ck_assert_int_eq(sqlite3_step(stmt), SQLITE_ROW);
    const void *blob = sqlite3_column_blob(stmt, 0);
    int blob_len = sqlite3_column_bytes(stmt, 0);
    ck_assert_ptr_nonnull(blob);
    ck_assert_int_gt(blob_len, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(raw);

    /* Third SM: original password again -> load now works, original content
     * preserved (not overwritten by the wrong-password open). */
    sm = session_manager_create(tmpdir, "right_password");
    ck_assert_ptr_nonnull(sm);
    s = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(s);
    ck_assert_int_eq(s->messages_count, 1);
    ck_assert_str_eq(s->messages[0].content, "secret");
    ck_assert_str_eq(s->title, "decrypt_test_title");
    session_free(s);
    free(sid);
    session_manager_free(sm);

    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int syst = system(rm);
    (void)syst;
}
END_TEST

START_TEST(test_session_list_alloc_fail_mid)
{
    char tmpdir[] = "/tmp/test_sm_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));

    SessionManager *sm = session_manager_create(tmpdir, "test_password");
    ck_assert_ptr_nonnull(sm);

    Session *s1 = session_manager_create_session(sm, "session one");
    ck_assert_ptr_nonnull(s1);
    session_free(s1);

    Session *s2 = session_manager_create_session(sm, "session two");
    ck_assert_ptr_nonnull(s2);
    session_free(s2);

    /* B6 regression: list_sessions used to silently truncate on str_dup
     * failure (returning partial results + leaking the new buffers). After
     * the fix it returns NULL instead. The test asserts NULL. */
    session_manager_test_set_alloc_fail(4);
    SessionList *list = session_manager_list_sessions(sm);
    ck_assert_ptr_null(list);

    /* Reset and confirm a normal listing still returns both rows. */
    session_manager_test_set_alloc_fail(-1);
    list = session_manager_list_sessions(sm);
    ck_assert_ptr_nonnull(list);
    ck_assert_int_eq(list->count, 2);
    session_list_free(list);

    session_manager_free(sm);

    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int rc = system(rm);
    (void)rc;
}
END_TEST

/* C8 regression: import_session used to do a `load_session` precheck and then
 * call session_manager_save_session (INSERT OR REPLACE) as two separate mutex
 * acquisitions. Between them another writer could insert the same id and the
 * import would silently overwrite. The fix makes the existence-check atomic
 * with the insert (`INSERT ... ON CONFLICT(id) DO NOTHING`). This test
 * validates the contract this fix preserves: importing a JSON document whose
 * id already exists in the DB (a) is rejected (returns NULL) and (b) does NOT
 * overwrite the existing row's content. The TOCTOU window itself is not
 * directly exercisable here (would need two coordinated threads), but the
 * contract holds at single-thread level under the new code, and structurally
 * the SQL is atomic so the race is closed. */
START_TEST(test_import_rejects_duplicate_id_preserves_existing)
{
    char tmpdir[] = "/tmp/test_sm_import_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));

    SessionManager *sm = session_manager_create(tmpdir, "test_password");
    ck_assert_ptr_nonnull(sm);

    /* Stage the DB with one session under id "DUP-ID" containing one message.
     * `session_manager_create_session` saves under a random id; we rename then
     * re-save under "DUP-ID" so the row is persisted with the intended PK. */
    Session *orig = session_manager_create_session(sm, "original title");
    ck_assert_ptr_nonnull(orig);
    free(orig->id);
    orig->id = str_dup("DUP-ID");
    ck_assert_ptr_nonnull(orig->id);
    ck_assert_int_eq(session_manager_save_session(sm, orig), 0);
    ck_assert_int_eq(session_manager_add_message(sm, "DUP-ID", "user",
                                                 "original message body",
                                                 NULL, NULL), 0);
    session_free(orig);

    /* Confirm the row is there and round-trips. */
    Session *loaded = session_manager_load_session(sm, "DUP-ID");
    ck_assert_ptr_nonnull(loaded);
    ck_assert_int_eq(loaded->messages_count, 1);
    ck_assert_str_eq(loaded->messages[0].content, "original message body");
    session_free(loaded);

    /* Build a JSON document with the SAME id but different content. */
    char json[] =
        "{"
        "\"id\":\"DUP-ID\","
        "\"title\":\"imposter title\","
        "\"title_generation_attempted\":7,"
        "\"created_at\":\"2030-01-01T00:00:00\","
        "\"messages\":["
        "  {\"role\":\"user\",\"content\":\"imposter message\"}"
        "]"
        "}";
    Session *imported = session_manager_import_session(sm, json);

    /* Duplicate id must be rejected — contract preserved under both old and
     * new code at the single-thread level. The NEW code additionally makes
     * this rejection atomic at the SQL level so a concurrent writer cannot
     * squeeze in between the check and the insert. */
    ck_assert_ptr_null(imported);

    /* Reload and confirm the ORIGINAL row survived untouched. */
    Session *survivor = session_manager_load_session(sm, "DUP-ID");
    ck_assert_ptr_nonnull(survivor);
    ck_assert_int_eq(survivor->messages_count, 1);
    ck_assert_str_eq(survivor->messages[0].content, "original message body");
    ck_assert_str_eq(survivor->title, "original title");
    session_free(survivor);

    /* Sanity: a unique-id import still succeeds. */
    char json2[] =
        "{"
        "\"id\":\"UNIQUE-ID\","
        "\"title\":\"fresh import\","
        "\"messages\":["
        "  {\"role\":\"user\",\"content\":\"hello world\"}"
        "]"
        "}";
    Session *fresh = session_manager_import_session(sm, json2);
    ck_assert_ptr_nonnull(fresh);
    ck_assert_str_eq(fresh->id, "UNIQUE-ID");
    session_free(fresh);

    Session *loaded2 = session_manager_load_session(sm, "UNIQUE-ID");
    ck_assert_ptr_nonnull(loaded2);
    ck_assert_int_eq(loaded2->messages_count, 1);
    ck_assert_str_eq(loaded2->messages[0].content, "hello world");
    session_free(loaded2);

    session_manager_free(sm);

    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int rc = system(rm);
    (void)rc;
}
END_TEST

/* C13 regression: `session_manager_save_session` used to let each
 * `encryption_encrypt` NULL return fall through to the `else
 * sqlite3_bind_null(...)` branch — the row got written with SQL NULL,
 * sqlite3_step returned SQLITE_DONE, and save reported 0 (success). On
 * the next load the column was undecryptable NULL (per C5 fix:
 * load_session then returns NULL), so the operator saw "session not
 * found" while the prior (potentially recoverable) blob had been
 * overwritten by SQL NULL. Irreversible silent permanent data loss.
 *
 * The fix adds a `*-enc == NULL` check after each encryption_encrypt call
 * (and a separate OOM-check on the serialize step) and refuses the save
 * with -1, leaving the prior row untouched.
 *
 * This test stages a session with a known messages blob in the DB, then
 * forces the Nth encryption_encrypt call to return NULL via the
 * SESSION_MANAGER_TEST fault-injection knob, and asserts:
 *   (a) save_session returns -1,
 *   (b) the prior row's messages blob is byte-identical to what we staged
 *       (no overwrite with NULL), and
 *   (c) the row still loads correctly (decryption round-trips), proving
 *       we did not silently destroy recoverable data. */
START_TEST(test_save_aborts_when_encrypt_fails_preserves_row)
{
    char tmpdir[] = "/tmp/test_sm_encfail_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));

    SessionManager *sm = session_manager_create(tmpdir, "test_password");
    ck_assert_ptr_nonnull(sm);

    /* Stage a row with one message so messages_encrypted is non-NULL. */
    Session *s = session_manager_create_session(sm, "title one");
    ck_assert_ptr_nonnull(s);
    ck_assert_int_eq(session_manager_add_message(sm, s->id,
                                                 "user", "body alpha",
                                                 NULL, NULL), 0);
    session_free(s);

    /* Re-load and capture the persisted messages blob bytes via raw SQL so
     * we can compare after the failing save attempt. We need to read the
     * actual ciphertext blob bytes — encryption_decrypt would also prove
     * preservation but a byte-level diff is stricter (rules out partial
     * overwrite). */
    sqlite3 *raw = NULL;
    ck_assert_int_eq(sqlite3_open_v2(
        sqlite3_db_filename(sm->db, "main"), &raw,
        SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    sqlite3_stmt *sel = NULL;
    ck_assert_int_eq(sqlite3_prepare_v2(raw,
        "SELECT messages_encrypted FROM agent_sessions LIMIT 1",
        -1, &sel, NULL), SQLITE_OK);
    ck_assert_int_eq(sqlite3_step(sel), SQLITE_ROW);
    int blob_bytes = sqlite3_column_bytes(sel, 0);
    ck_assert_int_gt(blob_bytes, 0);
    unsigned char *orig_blob = malloc((size_t)blob_bytes);
    ck_assert_ptr_nonnull(orig_blob);
    memcpy(orig_blob, sqlite3_column_blob(sel, 0), (size_t)blob_bytes);
    sqlite3_finalize(sel);
    sqlite3_close(raw);

    /* Build a session-state change to save: change the title so save_session
     * would actually produce a different row if it succeeded. Title-only
     * mutation means a single encryption_encrypt call (for the changed
     * title) — the 1st call in save_session_core. Forcing _that_ to fail
     * exercises the title branch of C13. */
    /* Load by id directly: we saved the staged row above, get its id. */
    sqlite3 *r2 = NULL;
    ck_assert_int_eq(sqlite3_open_v2(
        sqlite3_db_filename(sm->db, "main"), &r2,
        SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    sqlite3_stmt *idstmt = NULL;
    ck_assert_int_eq(sqlite3_prepare_v2(r2, "SELECT id FROM agent_sessions LIMIT 1",
                                       -1, &idstmt, NULL), SQLITE_OK);
    ck_assert_int_eq(sqlite3_step(idstmt), SQLITE_ROW);
    const char *row_id = (const char *)sqlite3_column_text(idstmt, 0);
    ck_assert_ptr_nonnull(row_id);
    char id_copy[128];
    snprintf(id_copy, sizeof(id_copy), "%s", row_id);
    sqlite3_finalize(idstmt);
    sqlite3_close(r2);

    Session *loaded = session_manager_load_session(sm, id_copy);
    ck_assert_ptr_nonnull(loaded);
    free(loaded->title);
    loaded->title = str_dup("title two");

    /* Force the FIRST encryption_encrypt call (title encryption) to return
     * NULL. */
    session_manager_test_set_encrypt_fail(1);
    int rc = session_manager_save_session(sm, loaded);
    ck_assert_int_eq(rc, -1);

    /* Reset the fault-injection knob and reload: the original title must
     * survive, original messages must survive. */
    session_manager_test_set_encrypt_fail(-1);
    Session *verify = session_manager_load_session(sm, id_copy);
    ck_assert_ptr_nonnull(verify);
    ck_assert_str_eq(verify->title, "title one");
    ck_assert_int_eq(verify->messages_count, 1);
    ck_assert_str_eq(verify->messages[0].content, "body alpha");
    session_free(verify);
    session_free(loaded);

    /* Also verify the raw bytes were preserved (no overwrite with NULL). */
    sqlite3 *r3 = NULL;
    ck_assert_int_eq(sqlite3_open_v2(
        sqlite3_db_filename(sm->db, "main"), &r3,
        SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    sqlite3_stmt *sel2 = NULL;
    ck_assert_int_eq(sqlite3_prepare_v2(r3,
        "SELECT messages_encrypted FROM agent_sessions WHERE id=?",
        -1, &sel2, NULL), SQLITE_OK);
    ck_assert_int_eq(sqlite3_bind_text(sel2, 1, id_copy, -1, SQLITE_TRANSIENT),
                     SQLITE_OK);
    ck_assert_int_eq(sqlite3_step(sel2), SQLITE_ROW);
    int blob_bytes2 = sqlite3_column_bytes(sel2, 0);
    ck_assert_int_eq(blob_bytes2, blob_bytes);
    ck_assert_int_eq(memcmp(sqlite3_column_blob(sel2, 0), orig_blob,
                            (size_t)blob_bytes), 0);
    sqlite3_finalize(sel2);
    sqlite3_close(r3);
    free(orig_blob);

    session_manager_free(sm);

    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int sysrc = system(rm);
    (void)sysrc;
}
END_TEST

START_TEST(test_create_creates_missing_parent_dirs)
{
    /* Regression: session_manager_create used to mkdir the data dir only
     * AFTER init_encryption, so a fresh HOME without ~/.config failed with
     * "failed to create salt file". Data dir must work when every parent
     * level is missing. */
    char parent[] = "/tmp/test_sm_mkdir_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(parent));

    char data_dir[4096];
    snprintf(data_dir, sizeof(data_dir), "%s/missing/sub/echo-ai", parent);

    SessionManager *sm = session_manager_create(data_dir, "pw");
    ck_assert_ptr_nonnull(sm);
    session_manager_free(sm);

    char salt_path[8192];
    snprintf(salt_path, sizeof(salt_path), "%s/salt", data_dir);
    ck_assert_int_eq(access(salt_path, F_OK), 0);
}
END_TEST

START_TEST(test_recovery_restores_backup_when_new_salt_was_not_created)
{
    char tmpdir[] = "/tmp/test_sm_missing_new_salt_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    SessionManager *sm = session_manager_create(tmpdir, "password");
    ck_assert_ptr_nonnull(sm);
    session_manager_free(sm);

    char salt_path[4096];
    char old_salt_path[4096];
    char marker_path[4096];
    ck_assert_int_lt(snprintf(salt_path, sizeof(salt_path), "%s/salt", tmpdir),
                     (int)sizeof(salt_path));
    ck_assert_int_lt(snprintf(old_salt_path, sizeof(old_salt_path),
                              "%s/salt.old", tmpdir),
                     (int)sizeof(old_salt_path));
    ck_assert_int_lt(snprintf(marker_path, sizeof(marker_path),
                              "%s/.changing_pwd", tmpdir),
                     (int)sizeof(marker_path));
    ck_assert_int_eq(rename(salt_path, old_salt_path), 0);
    write_test_file(marker_path, (const unsigned char *)"pending\n", 8);

    sm = session_manager_create(tmpdir, "password");
    ck_assert_ptr_nonnull(sm);
    ck_assert_int_eq(access(salt_path, F_OK), 0);
    ck_assert_int_eq(access(old_salt_path, F_OK), -1);
    ck_assert_int_eq(access(marker_path, F_OK), -1);
    session_manager_free(sm);

    char command[4096];
    ck_assert_int_lt(snprintf(command, sizeof(command), "rm -rf %s", tmpdir),
                     (int)sizeof(command));
    ck_assert_int_eq(system(command), 0);
}
END_TEST

START_TEST(test_first_run_verifier_failure_removes_orphan_salt)
{
    char tmpdir[] = "/tmp/test_sm_first_run_cleanup_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char verifier_path[4096];
    char salt_path[4096];
    ck_assert_int_lt(snprintf(verifier_path, sizeof(verifier_path),
                              "%s/.verifier", tmpdir),
                     (int)sizeof(verifier_path));
    ck_assert_int_lt(snprintf(salt_path, sizeof(salt_path), "%s/salt", tmpdir),
                     (int)sizeof(salt_path));
    ck_assert_int_eq(mkdir(verifier_path, 0700), 0);

    ck_assert_ptr_null(session_manager_create(tmpdir, "password"));
    ck_assert_int_eq(access(salt_path, F_OK), -1);
    ck_assert_int_eq(rmdir(verifier_path), 0);
    SessionManager *sm = session_manager_create(tmpdir, "password");
    ck_assert_ptr_nonnull(sm);
    session_manager_free(sm);

    char command[4096];
    ck_assert_int_lt(snprintf(command, sizeof(command), "rm -rf %s", tmpdir),
                     (int)sizeof(command));
    ck_assert_int_eq(system(command), 0);
}
END_TEST

/* ---------------------------------------------------------------------------
 * Branch support (fork / switch / branch_info) — BRANCHING_IMPLEMENTATION_PLAN.md §6
 * --------------------------------------------------------------------------- */

static SessionManager *branch_sm_create(char *tmpdir, char **sid_out)
{
    SessionManager *sm = session_manager_create(tmpdir, "pw");
    ck_assert_ptr_nonnull(sm);
    Session *s = session_manager_create_session(sm, "branch test");
    ck_assert_ptr_nonnull(s);
    *sid_out = str_dup(s->id);
    ck_assert_ptr_nonnull(*sid_out);
    session_free(s);
    return sm;
}

static void branch_rm(const char *tmpdir)
{
    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int rc = system(rm);
    (void)rc;
}

static void branch_add_msgs(SessionManager *sm, const char *sid,
                            const char *role1, const char *c1,
                            const char *role2, const char *c2)
{
    ck_assert_int_eq(session_manager_add_message(sm, sid, role1, c1,
                                                 NULL, NULL), 0);
    if (role2)
        ck_assert_int_eq(session_manager_add_message(sm, sid, role2, c2,
                                                     NULL, NULL), 0);
}

static void branch_free_result(SessionManagerForkResult *r)
{
    free(r->branch_id);
    free(r->fork_message_id);
    free(r->fork_group_id);
    message_clear(&r->fork_message);
    memset(r, 0, sizeof(*r));
}

/* Finds the branch record with `id` in the loaded session's metadata, or
 * NULL. Caller must not free the returned cJSON (owned by s->metadata). */
static cJSON *branch_find_record(Session *s, const char *id)
{
    cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
    cJSON *list = branches ? cJSON_GetObjectItem(branches, "list") : NULL;
    if (!list) return NULL;
    int n = cJSON_GetArraySize(list);
    for (int i = 0; i < n; i++)
    {
        cJSON *rec = cJSON_GetArrayItem(list, i);
        cJSON *rid = rec ? cJSON_GetObjectItem(rec, "id") : NULL;
        if (rid && rid->valuestring && strcmp(rid->valuestring, id) == 0)
            return rec;
    }
    return NULL;
}

/* Finds the branch_info entry with the given message_id, or NULL. */
static cJSON *branch_info_entry(cJSON *info, const char *message_id)
{
    int n = cJSON_GetArraySize(info);
    for (int i = 0; i < n; i++)
    {
        cJSON *e = cJSON_GetArrayItem(info, i);
        cJSON *mid = e ? cJSON_GetObjectItem(e, "message_id") : NULL;
        if (mid && mid->valuestring && strcmp(mid->valuestring, message_id) == 0)
            return e;
    }
    return NULL;
}

START_TEST(test_fork_creates_branch_record)
{
    char tmpdir[] = "/tmp/test_sm_fork_create_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "q1", "assistant", "a1");

    SessionManagerForkResult out = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0,
                                                 "q1 edited", &out), 0);
    ck_assert_ptr_nonnull(out.branch_id);
    ck_assert_ptr_nonnull(out.fork_message_id);
    ck_assert_ptr_nonnull(out.fork_group_id);
    ck_assert_str_ne(out.branch_id, out.fork_message_id);
    /* the returned fork message is the new live chain's tail */
    ck_assert_str_eq(out.fork_message.content, "q1 edited");
    ck_assert_str_eq(out.fork_message.id, out.fork_message_id);
    ck_assert_str_eq(out.fork_message.fork_group_id, out.fork_group_id);

    /* live chain: truncated to the fork point, tail replaced by the fork */
    Session *v = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_int_eq(v->messages_count, 1);
    ck_assert_str_eq(v->messages[0].content, "q1 edited");
    ck_assert_str_eq(v->messages[0].id, out.fork_message_id);
    ck_assert_str_eq(v->messages[0].fork_group_id, out.fork_group_id);

    /* snapshot record: original chain preserved, old tail intact, fork
     * point carries the SHARED group (so the old chain counts) */
    cJSON *rec = branch_find_record(v, out.branch_id);
    ck_assert_ptr_nonnull(rec);
    cJSON *rec_msgs = cJSON_GetObjectItem(rec, "messages");
    ck_assert_int_eq(cJSON_GetArraySize(rec_msgs), 2);
    cJSON *m0 = cJSON_GetArrayItem(rec_msgs, 0);
    ck_assert_str_eq(cJSON_GetObjectItem(m0, "content")->valuestring, "q1");
    ck_assert_str_eq(cJSON_GetObjectItem(m0, "fork_group_id")->valuestring,
                     out.fork_group_id);
    cJSON *m1 = cJSON_GetArrayItem(rec_msgs, 1);
    ck_assert_str_eq(cJSON_GetObjectItem(m1, "content")->valuestring, "a1");

    session_free(v);
    branch_free_result(&out);
    free(sid);
    session_manager_free(sm);
    branch_rm(tmpdir);
}
END_TEST

START_TEST(test_fork_by_message_id_resolves_index)
{
    char tmpdir[] = "/tmp/test_sm_fork_byid_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "user", "u1");
    ck_assert_int_eq(session_manager_add_message(sm, sid, "assistant", "a2",
                                                 NULL, NULL), 0);

    /* give the messages stable ids so an id-anchored fork is possible */
    Session *s = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(s);
    ck_assert_int_eq(s->messages_count, 3);
    s->messages[0].id = str_dup("m-0");
    s->messages[1].id = str_dup("m-1");
    s->messages[2].id = str_dup("m-2");
    ck_assert_int_eq(session_manager_save_session(sm, s), 0);
    session_free(s);

    /* index 99 is out of bounds — id must win and cut at m-1 */
    SessionManagerForkResult out = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, "m-1", 99,
                                                 "u1 edited", &out), 0);
    Session *v = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_int_eq(v->messages_count, 2);
    ck_assert_str_eq(v->messages[0].content, "u0");
    ck_assert_str_eq(v->messages[1].content, "u1 edited");
    ck_assert_str_eq(v->messages[1].id, out.fork_message_id);
    session_free(v);
    branch_free_result(&out);
    free(sid);
    session_manager_free(sm);
    branch_rm(tmpdir);
}
END_TEST

START_TEST(test_fork_unknown_message_id_falls_back_to_index)
{
    char tmpdir[] = "/tmp/test_sm_fork_fb_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    SessionManagerForkResult out = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, "no-such-id", 0,
                                                 "edited", &out), 0);
    Session *v = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_int_eq(v->messages_count, 1);
    ck_assert_str_eq(v->messages[0].content, "edited");
    session_free(v);
    branch_free_result(&out);
    free(sid);
    session_manager_free(sm);
    branch_rm(tmpdir);
}
END_TEST

START_TEST(test_regenerate_keeps_content)
{
    char tmpdir[] = "/tmp/test_sm_regen_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "q1", "assistant", "a1");

    /* content == NULL → the fork message keeps the original content */
    SessionManagerForkResult out = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, NULL,
                                                 &out), 0);
    ck_assert_str_eq(out.fork_message.content, "q1");
    Session *v = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_int_eq(v->messages_count, 1);
    ck_assert_str_eq(v->messages[0].content, "q1");
    ck_assert_str_eq(v->messages[0].id, out.fork_message_id);
    session_free(v);
    branch_free_result(&out);
    free(sid);
    session_manager_free(sm);
    branch_rm(tmpdir);
}
END_TEST

START_TEST(test_switch_branch_swaps_live_and_preserves_chains)
{
    char tmpdir[] = "/tmp/test_sm_switch_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    /* chain births are second-granularity; sleep so ordering is
     * deterministic (created_at is compared with strcmp) */
    sleep(1);
    SessionManagerForkResult f1 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "e1",
                                                 &f1), 0);
    sleep(1);
    SessionManagerForkResult f2 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "e2",
                                                 &f2), 0);

    /* switch back to the original chain (record br f1.branch_id) */
    ck_assert_int_eq(session_manager_switch_branch(sm, sid, f1.branch_id), 0);
    Session *v = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_int_eq(v->messages_count, 2);
    ck_assert_str_eq(v->messages[0].content, "u0");
    ck_assert_str_eq(v->messages[1].content, "a1");
    /* the switched-to record is popped (no chain is live while recorded) */
    ck_assert_ptr_null(branch_find_record(v, f1.branch_id));
    cJSON *branches = cJSON_GetObjectItem(v->metadata, "branches");
    cJSON *list = branches ? cJSON_GetObjectItem(branches, "list") : NULL;
    ck_assert_ptr_nonnull(list);
    ck_assert_int_eq(cJSON_GetArraySize(list), 2);
    /* the pre-switch live chain was snapshotted (preservation), and the
     * re-activated chain still counts its fork node: the pill must show
     * 1/3, not 1/1 (regression for the snapshot-before-group bug). Count
     * is 3 because f2 re-forked the f1 fork point (e1 carries fg1) and so
     * JOINED fg1 — one pill entry covering all three chains, never two */
    char *info = session_manager_branch_info_alloc(sm, sid);
    ck_assert_ptr_nonnull(info);
    cJSON *info_json = cJSON_Parse(info);
    free(info);
    ck_assert_ptr_nonnull(info_json);
    ck_assert_int_eq(cJSON_GetArraySize(info_json), 1);
    cJSON *e = branch_info_entry(info_json, f1.fork_message_id);
    ck_assert_ptr_nonnull(e);
    ck_assert_int_eq(cJSON_GetObjectItem(e, "count")->valueint, 3);
    ck_assert_int_eq(cJSON_GetObjectItem(e, "active")->valueint, 1);
    cJSON_Delete(info_json);
    session_free(v);

    /* switch to the middle chain (e1) — live chain again swaps */
    ck_assert_int_eq(session_manager_switch_branch(sm, sid, f2.branch_id), 0);
    Session *v2 = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(v2);
    ck_assert_int_eq(v2->messages_count, 1);
    ck_assert_str_eq(v2->messages[0].content, "e1");
    ck_assert_str_eq(v2->messages[0].id, f1.fork_message_id);
    session_free(v2);

    branch_free_result(&f1);
    branch_free_result(&f2);
    free(sid);
    session_manager_free(sm);
    branch_rm(tmpdir);
}
END_TEST

START_TEST(test_branch_info_counts_multifork_chain)
{
    char tmpdir[] = "/tmp/test_sm_info_multi_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");
    ck_assert_int_eq(session_manager_add_message(sm, sid, "user", "u2",
                                                 NULL, NULL), 0);
    ck_assert_int_eq(session_manager_add_message(sm, sid, "assistant", "a3",
                                                 NULL, NULL), 0);

    sleep(1);
    SessionManagerForkResult f1 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "e1",
                                                 &f1), 0);
    /* continue the forked chain, then fork a second point deeper in */
    ck_assert_int_eq(session_manager_add_message(sm, sid, "user", "u2b",
                                                 NULL, NULL), 0);
    ck_assert_int_eq(session_manager_add_message(sm, sid, "assistant", "a3b",
                                                 NULL, NULL), 0);
    sleep(1);
    SessionManagerForkResult f2 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 2, "e2",
                                                 &f2), 0);

    char *info = session_manager_branch_info_alloc(sm, sid);
    ck_assert_ptr_nonnull(info);
    cJSON *info_json = cJSON_Parse(info);
    free(info);
    ck_assert_ptr_nonnull(info_json);
    ck_assert_int_eq(cJSON_GetArraySize(info_json), 2);

    /* first fork point: live + original snapshot + first-fork snapshot */
    cJSON *e1 = branch_info_entry(info_json, f1.fork_message_id);
    ck_assert_ptr_nonnull(e1);
    ck_assert_int_eq(cJSON_GetObjectItem(e1, "count")->valueint, 3);
    ck_assert_int_eq(cJSON_GetObjectItem(e1, "active")->valueint, 3);
    /* both non-live chains carry records, ordered by creation */
    cJSON *e1_ids = cJSON_GetObjectItem(e1, "branch_ids");
    ck_assert_ptr_nonnull(e1_ids);
    ck_assert_int_eq(cJSON_GetArraySize(e1_ids), 2);
    ck_assert_str_eq(cJSON_GetArrayItem(e1_ids, 0)->valuestring,
                     f1.branch_id);

    /* second fork point: live + second-fork snapshot (original chain does
     * not contain it) */
    cJSON *e2 = branch_info_entry(info_json, f2.fork_message_id);
    ck_assert_ptr_nonnull(e2);
    ck_assert_int_eq(cJSON_GetObjectItem(e2, "count")->valueint, 2);
    ck_assert_int_eq(cJSON_GetObjectItem(e2, "active")->valueint, 2);
    cJSON *e2_ids = cJSON_GetObjectItem(e2, "branch_ids");
    ck_assert_ptr_nonnull(e2_ids);
    ck_assert_int_eq(cJSON_GetArraySize(e2_ids), 1);
    ck_assert_str_eq(cJSON_GetArrayItem(e2_ids, 0)->valuestring,
                     f2.branch_id);
    cJSON_Delete(info_json);

    branch_free_result(&f1);
    branch_free_result(&f2);
    free(sid);
    session_manager_free(sm);
    branch_rm(tmpdir);
}
END_TEST

START_TEST(test_old_session_without_branches_reports_empty)
{
    char tmpdir[] = "/tmp/test_sm_info_legacy_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    char *info = session_manager_branch_info_alloc(sm, sid);
    ck_assert_ptr_nonnull(info);
    ck_assert_str_eq(info, "[]");
    free(info);

    free(sid);
    session_manager_free(sm);
    branch_rm(tmpdir);
}
END_TEST

START_TEST(test_fork_allocation_failure_leaves_session_unchanged)
{
    char tmpdir[] = "/tmp/test_sm_fork_oom_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "q1", "assistant", "a1");

    /* Fail the 5th str_dup: load_session_locked does 4 (id, empty title,
     * created_at, decrypted title) and the fork path's first is
     * branch_active_created_at — before the snapshot/truncate/commit, so
     * nothing may change on disk (all-or-nothing per plan §3.2). */
    SessionManagerForkResult out = {0};
    session_manager_test_set_alloc_fail(5);
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "edited",
                                                 &out), -1);
    session_manager_test_set_alloc_fail(-1);
    /* *out zeroed, no dangling pointers */
    ck_assert_ptr_null(out.branch_id);
    ck_assert_ptr_null(out.fork_message_id);
    ck_assert_ptr_null(out.fork_group_id);
    ck_assert_ptr_null(out.fork_message.content);

    /* session unchanged on disk: original chain, no groups, no branches */
    Session *v = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_int_eq(v->messages_count, 2);
    ck_assert_str_eq(v->messages[0].content, "q1");
    ck_assert_str_eq(v->messages[1].content, "a1");
    ck_assert_ptr_null(v->messages[0].fork_group_id);
    ck_assert_ptr_null(cJSON_GetObjectItem(v->metadata, "branches"));
    session_free(v);

    /* normal operation restored after the fault injection is lifted */
    SessionManagerForkResult out2 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "edited",
                                                 &out2), 0);
    Session *v2 = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(v2);
    ck_assert_int_eq(v2->messages_count, 1);
    ck_assert_str_eq(v2->messages[0].content, "edited");
    session_free(v2);

    branch_free_result(&out2);
    free(sid);
    session_manager_free(sm);
    branch_rm(tmpdir);
}
END_TEST

START_TEST(test_switch_away_and_back_preserves_branch_data)
{
    /* Regression for the switch-back data-loss bug (found via live smoke):
     * branch_record_snapshot_live used to REPLACE the record whose
     * anchor_message_id matched the live chain's last fork id — and
     * sibling chains of one fork share the fork point's id, so switching
     * back clobbered the record of the chain the user had just left
     * (history reverted to the unedited content, pill collapsed to
     * count=1). Records must be appended, never replaced by anchor. */
    char tmpdir[] = "/tmp/test_sm_switch_back_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    SessionManagerForkResult f1 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "edited",
                                                 &f1), 0);

    /* switch to the old chain */
    ck_assert_int_eq(session_manager_switch_branch(sm, sid, f1.branch_id), 0);
    Session *v1 = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(v1);
    ck_assert_int_eq(v1->messages_count, 2);
    ck_assert_str_eq(v1->messages[0].content, "u0");
    session_free(v1);

    /* the edited chain must still exist as a record, and the pill count
     * must still span both chains */
    char *info1 = session_manager_branch_info_alloc(sm, sid);
    ck_assert_ptr_nonnull(info1);
    cJSON *j1 = cJSON_Parse(info1);
    free(info1);
    ck_assert_ptr_nonnull(j1);
    ck_assert_int_eq(cJSON_GetArraySize(j1), 1);
    cJSON *e1 = branch_info_entry(j1, f1.fork_message_id);
    ck_assert_ptr_nonnull(e1);
    ck_assert_int_eq(cJSON_GetObjectItem(e1, "count")->valueint, 2);
    ck_assert_int_eq(cJSON_GetObjectItem(e1, "active")->valueint, 1);
    cJSON *e1_ids = cJSON_GetObjectItem(e1, "branch_ids");
    ck_assert_ptr_nonnull(e1_ids);
    ck_assert_int_eq(cJSON_GetArraySize(e1_ids), 1);
    char *other_id = str_dup(cJSON_GetArrayItem(e1_ids, 0)->valuestring);
    ck_assert_ptr_nonnull(other_id);
    cJSON_Delete(j1);

    /* switch back: the edited content must survive */
    ck_assert_int_eq(session_manager_switch_branch(sm, sid, other_id), 0);
    free(other_id);
    Session *v2 = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(v2);
    ck_assert_int_eq(v2->messages_count, 1);
    ck_assert_str_eq(v2->messages[0].content, "edited");

    char *info2 = session_manager_branch_info_alloc(sm, sid);
    ck_assert_ptr_nonnull(info2);
    cJSON *j2 = cJSON_Parse(info2);
    free(info2);
    ck_assert_ptr_nonnull(j2);
    cJSON *e2 = branch_info_entry(j2, f1.fork_message_id);
    ck_assert_ptr_nonnull(e2);
    ck_assert_int_eq(cJSON_GetObjectItem(e2, "count")->valueint, 2);
    ck_assert_int_eq(cJSON_GetObjectItem(e2, "active")->valueint, 2);
    cJSON_Delete(j2);
    session_free(v2);

    branch_free_result(&f1);
    free(sid);
    session_manager_free(sm);
    branch_rm(tmpdir);
}
END_TEST

START_TEST(test_refork_joins_existing_group)
{
    /* Re-editing the same message must JOIN the fork point's existing
     * fork_group_id: a fresh group would orphan the earlier fork's chains
     * from the pill (its records carry the old group) and the count would
     * not span the whole family. */
    char tmpdir[] = "/tmp/test_sm_refork_group_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    SessionManagerForkResult f1 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "e1",
                                                 &f1), 0);
    /* the fork point now carries f1's group — a re-fork joins it */
    SessionManagerForkResult f2 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "e2",
                                                 &f2), 0);
    ck_assert_str_eq(f2.fork_group_id, f1.fork_group_id);

    char *info = session_manager_branch_info_alloc(sm, sid);
    ck_assert_ptr_nonnull(info);
    cJSON *info_json = cJSON_Parse(info);
    free(info);
    ck_assert_ptr_nonnull(info_json);
    /* exactly one pill entry spans all three chains */
    ck_assert_int_eq(cJSON_GetArraySize(info_json), 1);
    cJSON *e = branch_info_entry(info_json, f2.fork_message_id);
    ck_assert_ptr_nonnull(e);
    ck_assert_int_eq(cJSON_GetObjectItem(e, "count")->valueint, 3);
    ck_assert_int_eq(cJSON_GetObjectItem(e, "active")->valueint, 3);
    cJSON_Delete(info_json);

    branch_free_result(&f1);
    branch_free_result(&f2);
    free(sid);
    session_manager_free(sm);
    branch_rm(tmpdir);
}
END_TEST

START_TEST(test_fork_same_second_orders_active)
{
    /* Millisecond-granularity chain births: with second-granularity
     * timestamps a fork right after session creation produced active=1
     * (the record's created_at and the live chain's shared one second and
     * strcmp ordering won). Deliberately NO sleep here — the fork happens
     * milliseconds after session creation, which the old code misordered
     * and the ms-resolution code orders deterministically. */
    char tmpdir[] = "/tmp/test_sm_ms_order_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    SessionManagerForkResult f1 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "e1",
                                                 &f1), 0);

    char *info = session_manager_branch_info_alloc(sm, sid);
    ck_assert_ptr_nonnull(info);
    cJSON *info_json = cJSON_Parse(info);
    free(info);
    ck_assert_ptr_nonnull(info_json);
    cJSON *e = branch_info_entry(info_json, f1.fork_message_id);
    ck_assert_ptr_nonnull(e);
    ck_assert_int_eq(cJSON_GetObjectItem(e, "count")->valueint, 2);
    ck_assert_int_eq(cJSON_GetObjectItem(e, "active")->valueint, 2);
    cJSON_Delete(info_json);

    branch_free_result(&f1);
    free(sid);
    session_manager_free(sm);
    branch_rm(tmpdir);
}
END_TEST

START_TEST(test_tag_message_marks_fork_point)
{
    char tmpdir[] = "/tmp/test_sm_tag_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    char *tagged = session_manager_tag_message(sm, sid, 1, "fg_tag");
    ck_assert_ptr_nonnull(tagged);
    ck_assert_str_ne(tagged, "");

    Session *v = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_int_eq(v->messages_count, 2);
    ck_assert_str_eq(v->messages[1].role, "assistant");
    ck_assert_str_eq(v->messages[1].id, tagged);
    ck_assert_str_eq(v->messages[1].fork_group_id, "fg_tag");
    /* the user message is untouched */
    ck_assert_ptr_null(v->messages[0].id);
    ck_assert_ptr_null(v->messages[0].fork_group_id);
    char *old_id = str_dup(tagged);
    ck_assert_ptr_nonnull(old_id);
    session_free(v);
    free(tagged);

    /* re-tagging a message that already carries a group keeps the group */
    char *tagged2 = session_manager_tag_message(sm, sid, 1, "fg_other");
    ck_assert_ptr_nonnull(tagged2);
    Session *v2 = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(v2);
    ck_assert_str_eq(v2->messages[1].fork_group_id, "fg_tag");
    ck_assert_str_ne(v2->messages[1].id, old_id);
    session_free(v2);
    free(old_id);
    free(tagged2);

    /* out-of-range index: NULL, nothing changes */
    ck_assert_ptr_null(session_manager_tag_message(sm, sid, 5, "fg_x"));
    ck_assert_ptr_null(session_manager_tag_message(sm, sid, -1, "fg_x"));

    free(sid);
    session_manager_free(sm);
    branch_rm(tmpdir);
}
END_TEST

START_TEST(test_tag_message_oom_leaves_session_unchanged)
{
    char tmpdir[] = "/tmp/test_sm_tag_oom_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    /* The 5th str_dup: load_session_locked does 4 (id, empty title,
     * created_at, decrypted title) and the group dup is tag_message's
     * first — it must fail BEFORE anything is committed. */
    session_manager_test_set_alloc_fail(5);
    ck_assert_ptr_null(session_manager_tag_message(sm, sid, 1, "fg_tag"));
    session_manager_test_set_alloc_fail(-1);

    Session *v = session_manager_load_session(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_ptr_null(v->messages[1].id);
    ck_assert_ptr_null(v->messages[1].fork_group_id);
    session_free(v);

    /* normal operation restored after the fault injection is lifted */
    char *tagged = session_manager_tag_message(sm, sid, 1, "fg_tag");
    ck_assert_ptr_nonnull(tagged);
    free(tagged);

    free(sid);
    session_manager_free(sm);
    branch_rm(tmpdir);
}
END_TEST

Suite *session_mgr_suite(void)
{
    Suite *s = suite_create("SessionManager");

    TCase *tc_encrypt = tcase_create("Encryption");
    tcase_set_timeout(tc_encrypt, 120);
    tcase_add_test(tc_encrypt, test_title_is_encrypted_at_rest);
    tcase_add_test(tc_encrypt, test_load_returns_null_on_decrypt_failure_preserves_row);
    tcase_add_test(tc_encrypt, test_save_aborts_when_encrypt_fails_preserves_row);
    tcase_add_test(tc_encrypt,
                   test_missing_verifier_never_accepts_or_commits_a_password);
    suite_add_tcase(s, tc_encrypt);

    TCase *tc_oauth = tcase_create("ProviderOAuth");
    tcase_set_timeout(tc_oauth, 180);
    tcase_add_test(tc_oauth, test_provider_oauth_encrypted_roundtrip_and_delete);
    tcase_add_test(tc_oauth,
                   test_provider_oauth_typed_failures_preserve_credentials);
    tcase_add_test(tc_oauth,
                   test_password_change_migrates_oauth_and_recovers_post_commit);
    tcase_add_test(tc_oauth,
                   test_password_change_retries_verifier_swap_in_process);
    tcase_add_test(tc_oauth,
                   test_password_change_rolls_back_on_malformed_oauth_row);
    tcase_add_test(tc_oauth,
                   test_legacy_post_commit_recovery_uses_encrypted_row_evidence);
    suite_add_tcase(s, tc_oauth);

    TCase *tc_crud = tcase_create("CRUD");
    tcase_set_timeout(tc_crud, 120);
    tcase_add_test(tc_crud, test_user_memory_table_ready_after_sm_create);
    tcase_add_test(tc_crud, test_save_session_bind_failure_aborts);
    tcase_add_test(tc_crud, test_purge_sessions_bind_failure_returns_error);
    tcase_add_test(tc_crud, test_purge_sessions_rejects_bad_days);
    tcase_add_test(tc_crud, test_delete_session_distinguishes_missing);
    tcase_add_test(tc_crud, test_empty_or_null_id_refused);
    tcase_add_test(tc_crud, test_session_list_alloc_fail_mid);
    tcase_add_test(tc_crud, test_session_list_realloc_failures_are_safe);
    tcase_add_test(tc_crud, test_import_rejects_duplicate_id_preserves_existing);
    tcase_add_test(tc_crud, test_create_creates_missing_parent_dirs);
    tcase_add_test(tc_crud,
                   test_recovery_restores_backup_when_new_salt_was_not_created);
    tcase_add_test(tc_crud,
                   test_first_run_verifier_failure_removes_orphan_salt);
    tcase_add_test(tc_crud,
                   test_provider_state_survives_encrypted_reload_and_retained_owner);
    suite_add_tcase(s, tc_crud);

    TCase *tc_branches = tcase_create("Branches");
    tcase_set_timeout(tc_branches, 180);
    tcase_add_test(tc_branches, test_fork_creates_branch_record);
    tcase_add_test(tc_branches, test_fork_by_message_id_resolves_index);
    tcase_add_test(tc_branches, test_fork_unknown_message_id_falls_back_to_index);
    tcase_add_test(tc_branches, test_regenerate_keeps_content);
    tcase_add_test(tc_branches, test_switch_branch_swaps_live_and_preserves_chains);
    tcase_add_test(tc_branches, test_branch_info_counts_multifork_chain);
    tcase_add_test(tc_branches, test_old_session_without_branches_reports_empty);
    tcase_add_test(tc_branches,
                   test_fork_allocation_failure_leaves_session_unchanged);
    tcase_add_test(tc_branches,
                   test_switch_away_and_back_preserves_branch_data);
    tcase_add_test(tc_branches, test_refork_joins_existing_group);
    tcase_add_test(tc_branches, test_fork_same_second_orders_active);
    tcase_add_test(tc_branches, test_tag_message_marks_fork_point);
    tcase_add_test(tc_branches,
                   test_tag_message_oom_leaves_session_unchanged);
    suite_add_tcase(s, tc_branches);

    return s;
}

int main(void)
{
    Suite *s = session_mgr_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
