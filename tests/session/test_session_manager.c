#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sqlite3.h>
#include "session/session_manager.h"
#include "session/encryption.h"
#include "session/memory.h"
#include "utils/string_utils.h"

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
    ck_assert_ptr_nonnull(sm);

    ck_assert_ptr_null(session_manager_load_session(sm, sid));

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

    session_manager_free(sm);

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

    char salt_path[4096];
    snprintf(salt_path, sizeof(salt_path), "%s/salt", data_dir);
    ck_assert_int_eq(access(salt_path, F_OK), 0);
}
END_TEST

Suite *session_mgr_suite(void)
{
    Suite *s = suite_create("SessionManager");

    TCase *tc_encrypt = tcase_create("Encryption");
    tcase_set_timeout(tc_encrypt, 30);
    tcase_add_test(tc_encrypt, test_title_is_encrypted_at_rest);
    tcase_add_test(tc_encrypt, test_load_returns_null_on_decrypt_failure_preserves_row);
    tcase_add_test(tc_encrypt, test_save_aborts_when_encrypt_fails_preserves_row);
    suite_add_tcase(s, tc_encrypt);

    TCase *tc_crud = tcase_create("CRUD");
    tcase_set_timeout(tc_crud, 30);
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
    suite_add_tcase(s, tc_crud);

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
