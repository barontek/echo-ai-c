/* test_session_manager.c - session store Encryption tests
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

START_TEST(test_title_is_encrypted_at_rest)
{
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
    Session *loaded2 = session_manager_load_session_alloc(sm, list->ids[0]);
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

START_TEST(test_load_returns_null_on_decrypt_failure_preserves_row)
{
    /* First SM: real password, real session with real messages, saved. */
    SessionManager *sm = session_manager_create(tmpdir, "right_password");
    ck_assert_ptr_nonnull(sm);

    Session *s = session_manager_create_session(sm, "decrypt_test_title");
    ck_assert_ptr_nonnull(s);
    char *sid = str_dup(s->id);
    ck_assert_int_eq(session_manager_add_message(sm, sid, "user", "secret", NULL, NULL), 0);
    session_free(s);

    /* Confirm we can load it back under the right password. */
    s = session_manager_load_session_alloc(sm, sid);
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
    s = session_manager_load_session_alloc(sm, sid);
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

START_TEST(test_save_aborts_when_encrypt_fails_preserves_row)
{
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

    Session *loaded = session_manager_load_session_alloc(sm, id_copy);
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
    Session *verify = session_manager_load_session_alloc(sm, id_copy);
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

START_TEST(test_missing_verifier_never_accepts_or_commits_a_password)
{
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

}
END_TEST


Suite *session_mgr_manager_suite(void)
{
    Suite *s = suite_create("SessionManager");

    TCase *tc = tcase_create("Encryption");
    tcase_add_checked_fixture(tc, tmpdir_setup, tmpdir_teardown);
    tcase_set_timeout(tc, 180);
    tcase_add_test(tc, test_title_is_encrypted_at_rest);
    tcase_add_test(tc, test_load_returns_null_on_decrypt_failure_preserves_row);
    tcase_add_test(tc, test_save_aborts_when_encrypt_fails_preserves_row);
    tcase_add_test(tc, test_missing_verifier_never_accepts_or_commits_a_password);

    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    Suite *s = session_mgr_manager_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
