/* test_session_oauth.c - session store ProviderOAuth tests
 * Split from test_session_manager.c (2026-08 file-length compliance);
 * shared fixtures live in test_session_fixture.c. Depends on: check,
 * the session store under SESSION_MANAGER_TEST.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "test_session_fixture.h"

/* Declared in migration.h under SESSION_MANAGER_TEST; the test binary
 * compiles migration.c with that define but does not include the header. */
extern void migration_test_set_rename_fail(int nth_rename);

START_TEST(test_provider_oauth_encrypted_roundtrip_and_delete)
{
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
}
END_TEST

START_TEST(test_provider_oauth_typed_failures_preserve_credentials)
{
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

}
END_TEST

START_TEST(test_password_change_migrates_oauth_and_recovers_post_commit)
{
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
    session = session_manager_load_session_alloc(sm, session_id);
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
    session = session_manager_load_session_alloc(sm, session_id);
    ck_assert_ptr_nonnull(session);
    ck_assert_str_eq(session->title, "surviving session");
    session_free(session);
    free(session_id);
    session_manager_free(sm);

}
END_TEST

START_TEST(test_password_change_retries_verifier_swap_in_process)
{
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
    session = session_manager_load_session_alloc(sm, session_id);
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

}
END_TEST

START_TEST(test_password_change_rolls_back_on_malformed_oauth_row)
{
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
    session = session_manager_load_session_alloc(sm, session_id);
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
    session = session_manager_load_session_alloc(sm, session_id);
    ck_assert_ptr_nonnull(session);
    ck_assert_str_eq(session->title, "rollback session");
    session_free(session);
    free(session_id);
    session_manager_free(sm);

}
END_TEST

START_TEST(test_legacy_post_commit_recovery_uses_encrypted_row_evidence)
{
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
    session = session_manager_load_session_alloc(sm, session_id);
    ck_assert_ptr_nonnull(session);
    ck_assert_str_eq(session->title, "legacy session");
    session_free(session);
    free(session_id);
    session_manager_free(sm);

}
END_TEST

START_TEST(test_migration_change_password_asprintf_failure_aborts_cleanly)
{
    for (int fail_at = 1; fail_at <= 3; fail_at++)
    {
        SessionManager *sm = session_manager_create(tmpdir, "old_password");
        ck_assert_ptr_nonnull(sm);
        Session *s = session_manager_create_session(sm, "mig test");
        ck_assert_ptr_nonnull(s);
        char *session_id = str_dup(s->id);
        ck_assert_ptr_nonnull(session_id);
        session_free(s);

        session_manager_test_set_asprintf_fail(fail_at);
        int rc = migration_change_password(sm, "new_password");
        session_manager_test_set_asprintf_fail(-1);
        ck_assert_int_eq(rc, -1);

        /* old password still opens the store and the session survives */
        session_manager_free(sm);
        sm = session_manager_create(tmpdir, "old_password");
        ck_assert_ptr_nonnull(sm);
        Session *loaded = session_manager_load_session_alloc(sm, session_id);
        ck_assert_ptr_nonnull(loaded);
        ck_assert_str_eq(loaded->title, "mig test");
        session_free(loaded);
        free(session_id);
        session_manager_free(sm);
    }
}
END_TEST


Suite *session_mgr_oauth_suite(void)
{
    Suite *s = suite_create("SessionManagerOauth");

    TCase *tc = tcase_create("ProviderOAuth");
    tcase_add_checked_fixture(tc, tmpdir_setup, tmpdir_teardown);
    tcase_set_timeout(tc, 180);
    tcase_add_test(tc, test_provider_oauth_encrypted_roundtrip_and_delete);
    tcase_add_test(tc, test_provider_oauth_typed_failures_preserve_credentials);
    tcase_add_test(tc, test_password_change_migrates_oauth_and_recovers_post_commit);
    tcase_add_test(tc, test_password_change_retries_verifier_swap_in_process);
    tcase_add_test(tc, test_password_change_rolls_back_on_malformed_oauth_row);
    tcase_add_test(tc, test_legacy_post_commit_recovery_uses_encrypted_row_evidence);
    tcase_add_test(tc, test_migration_change_password_asprintf_failure_aborts_cleanly);

    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    Suite *s = session_mgr_oauth_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
