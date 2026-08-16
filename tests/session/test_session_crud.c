/* test_session_crud.c - session store CRUD tests
 * Split from test_session_manager.c (2026-08 file-length compliance);
 * shared fixtures live in test_session_fixture.c. Depends on: check,
 * the session store under SESSION_MANAGER_TEST.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "test_session_fixture.h"

START_TEST(test_user_memory_table_ready_after_sm_create)
{
    SessionManager *sm = session_manager_create(tmpdir, "test-password");
    ck_assert_ptr_nonnull(sm);

    ck_assert_int_eq(memory_set(sm->db, "fact1", "value1"), 0);
    ck_assert_int_eq(memory_set(sm->db, "fact2", "value2"), 0);

    int count = 0;
    MemoryFact *facts = memory_list_all(sm->db, &count, NULL);
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

START_TEST(test_save_session_bind_failure_aborts)
{
    SessionManager *sm = session_manager_create(tmpdir, "test-password");
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

START_TEST(test_purge_sessions_bind_failure_returns_error)
{
    SessionManager *sm = session_manager_create(tmpdir, "test-password");
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

START_TEST(test_purge_sessions_rejects_bad_days)
{
    SessionManager *sm = session_manager_create(tmpdir, "test-password");
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

START_TEST(test_delete_session_distinguishes_missing)
{
    SessionManager *sm = session_manager_create(tmpdir, "test-password");
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

START_TEST(test_empty_or_null_id_refused)
{
    SessionManager *sm = session_manager_create(tmpdir, "test-password");
    ck_assert_ptr_nonnull(sm);

    /* load_session with empty id -> NULL (and no SQLite call is made) */
    ck_assert_ptr_null(session_manager_load_session_alloc(sm, ""));

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

START_TEST(test_session_list_realloc_failures_are_safe)
{
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

START_TEST(test_encryption_rejects_hmac_valid_bad_padding_token)
{
    unsigned char salt[16] = {1, 2, 3, 4, 5, 6, 7, 8,
                              9, 10, 11, 12, 13, 14, 15, 16};
    unsigned char pepper[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01, 0x02};
    EncryptionKey key;
    ck_assert_int_eq(encryption_key_derive("unit-test-pw", salt, sizeof(salt),
                                           pepper, sizeof(pepper), &key), 0);

    const char *plain = "hello world";
    int token_len = 0;
    unsigned char *token = encryption_encrypt(
        &key, (const unsigned char *)plain, (int)strlen(plain), &token_len);
    ck_assert_ptr_nonnull(token);
    ck_assert_int_gt(token_len, 1 + 8 + 16 + 32);

    /* Corrupt the last IV byte. The IV XORs into the first plaintext
     * block one-to-one (no ciphertext avalanche), so the final padding
     * byte becomes 0x05 ^ 0xFF = 0xFA — invalid PKCS7 padding with
     * certainty. Corrupting a ciphertext byte instead avalanches the
     * whole block, leaving the padding valid ~0.4% of the time
     * (sum of (1/256)^n over n = 1..16), which made this test flaky.
     * Token layout: version (1) | ts (8) | IV (16) | ciphertext | HMAC (32). */
    int iv_off = 1 + 8 + 16 - 1;
    token[iv_off] ^= 0xFF;

    /* Re-sign the corrupted token so the HMAC stays valid: the key's low
     * half signs (per encryption.h), and the HMAC covers everything up to
     * the trailing 32 bytes. */
    unsigned char hmac[32];
    unsigned int hmac_len = sizeof(hmac);
    HMAC(EVP_sha256(), key.key, 16, token, token_len - 32, hmac, &hmac_len);
    ck_assert_int_eq((int)hmac_len, 32);
    memcpy(token + token_len - 32, hmac, 32);

    int out_len = 0;
    unsigned char *dec = encryption_decrypt(&key, token, token_len, &out_len);
    ck_assert_ptr_null(dec);
    ck_assert_int_eq(out_len, 0);
    free(token);
}
END_TEST

START_TEST(test_encryption_rejects_future_timestamp_token)
{
    unsigned char salt[16] = {1, 2, 3, 4, 5, 6, 7, 8,
                              9, 10, 11, 12, 13, 14, 15, 16};
    unsigned char pepper[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01, 0x02};
    EncryptionKey key;
    ck_assert_int_eq(encryption_key_derive("unit-test-pw", salt, sizeof(salt),
                                           pepper, sizeof(pepper), &key), 0);

    const char *plain = "hello world";
    int token_len = 0;
    unsigned char *token = encryption_encrypt(
        &key, (const unsigned char *)plain, (int)strlen(plain), &token_len);
    ck_assert_ptr_nonnull(token);

    /* Regression: decrypt_fernet_token used to ignore the 8-byte BE
     * timestamp, so a forged clock was accepted. Rewrite the timestamp
     * to now+3600 and re-sign so the HMAC stays valid — rejection must
     * come from the freshness check alone, not from a broken MAC. */
    uint64_t future = (uint64_t)time(NULL) + 3600;
    for (int i = 0; i < 8; i++)
        token[1 + i] = (unsigned char)(future >> (56 - i * 8));

    unsigned char hmac[32];
    unsigned int hmac_len = sizeof(hmac);
    HMAC(EVP_sha256(), key.key, 16, token, token_len - 32, hmac, &hmac_len);
    ck_assert_int_eq((int)hmac_len, 32);
    memcpy(token + token_len - 32, hmac, 32);

    int out_len = 0;
    unsigned char *dec = encryption_decrypt(&key, token, token_len, &out_len);
    ck_assert_ptr_null(dec);
    ck_assert_int_eq(out_len, 0);

    /* The same token rewound to the past must decrypt fine: the check is
     * a future-clock guard (Fernet spec), not a TTL. */
    uint64_t past = (uint64_t)time(NULL) - 3600;
    for (int i = 0; i < 8; i++)
        token[1 + i] = (unsigned char)(past >> (56 - i * 8));
    HMAC(EVP_sha256(), key.key, 16, token, token_len - 32, hmac, &hmac_len);
    memcpy(token + token_len - 32, hmac, 32);
    dec = encryption_decrypt(&key, token, token_len, &out_len);
    ck_assert_ptr_nonnull(dec);
    ck_assert_str_eq((const char *)dec, plain);
    free(dec);
    free(token);
}
END_TEST

START_TEST(test_first_run_rejects_short_password)
{
    /* Regression: the TUI getpass path enforced no minimum at all and
     * the HTTP setup path allowed 4 characters. The single first-run
     * check in init_encryption must reject both. */
    ck_assert_ptr_null(session_manager_create(tmpdir, "1234567"));
    ck_assert_int_eq(access("/tmp/does-not-exist-salt", F_OK), -1);
    /* No key material may exist after a rejected first run. */
    char salt_path[512];
    snprintf(salt_path, sizeof(salt_path), "%s/salt", tmpdir);
    ck_assert_int_eq(access(salt_path, F_OK), -1);
    char pepper_path[512];
    snprintf(pepper_path, sizeof(pepper_path), "%s/.pepper", tmpdir);
    ck_assert_int_eq(access(pepper_path, F_OK), -1);

    SessionManager *sm = session_manager_create(tmpdir, "12345678");
    ck_assert_ptr_nonnull(sm);
    session_manager_free(sm);
}
END_TEST

START_TEST(test_vault_without_pepper_cannot_unlock)
{
    /* Regression: a vault whose .pepper file is missing must refuse to
     * initialize rather than derive a pepper-less key that silently
     * fails to decrypt every token. */
    SessionManager *sm = session_manager_create(tmpdir, "good-password");
    ck_assert_ptr_nonnull(sm);
    session_manager_free(sm);

    char pepper_path[512];
    snprintf(pepper_path, sizeof(pepper_path), "%s/.pepper", tmpdir);
    ck_assert_int_eq(unlink(pepper_path), 0);

    ck_assert_ptr_null(session_manager_create(tmpdir, "good-password"));
}
END_TEST

START_TEST(test_key_material_file_modes)
{
    /* Defense-in-depth: salt, pepper, verifier and the DB must be 0600
     * and the vault dir 0700 even under a permissive umask. */
    umask(0);
    SessionManager *sm = session_manager_create(tmpdir, "good-password");
    ck_assert_ptr_nonnull(sm);

    struct stat st;
    const char *paths[] = {
        "/salt", "/.pepper", "/.verifier", "/echo-ai.db"
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
    {
        char full[512];
        snprintf(full, sizeof(full), "%s%s", tmpdir, paths[i]);
        ck_assert_int_eq(stat(full, &st), 0);
        ck_assert_int_eq(st.st_mode & 0777, 0600);
    }
    ck_assert_int_eq(stat(tmpdir, &st), 0);
    ck_assert_int_eq(st.st_mode & 0777, 0700);
    session_manager_free(sm);
}
END_TEST

START_TEST(test_key_derive_pepper_changes_key)
{
    unsigned char salt[16] = {1, 2, 3, 4, 5, 6, 7, 8,
                              9, 10, 11, 12, 13, 14, 15, 16};
    unsigned char pepper_a[4] = {0x01, 0x02, 0x03, 0x04};
    unsigned char pepper_b[4] = {0x05, 0x06, 0x07, 0x08};
    EncryptionKey key_a, key_a2, key_b;

    ck_assert_int_eq(encryption_key_derive("unit-test-pw", salt, sizeof(salt),
                                           pepper_a, sizeof(pepper_a), &key_a), 0);
    ck_assert_int_eq(encryption_key_derive("unit-test-pw", salt, sizeof(salt),
                                           pepper_a, sizeof(pepper_a), &key_a2), 0);
    ck_assert_int_eq(encryption_key_derive("unit-test-pw", salt, sizeof(salt),
                                           pepper_b, sizeof(pepper_b), &key_b), 0);

    ck_assert_mem_eq(key_a.key, key_a2.key, sizeof(key_a.key));
    ck_assert_mem_ne(key_a.key, key_b.key, sizeof(key_a.key));

    /* Pepper-less derivation must also work (legacy callers) and must
     * differ from any peppered derivation. */
    EncryptionKey key_nop;
    ck_assert_int_eq(encryption_key_derive("unit-test-pw", salt, sizeof(salt),
                                           NULL, 0, &key_nop), 0);
    ck_assert_mem_ne(key_a.key, key_nop.key, sizeof(key_a.key));
}
END_TEST

START_TEST(test_import_rejects_duplicate_id_preserves_existing)
{
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
    Session *loaded = session_manager_load_session_alloc(sm, "DUP-ID");
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
    Session *imported = session_manager_import_session_new(sm, json);

    /* Duplicate id must be rejected — contract preserved under both old and
     * new code at the single-thread level. The NEW code additionally makes
     * this rejection atomic at the SQL level so a concurrent writer cannot
     * squeeze in between the check and the insert. */
    ck_assert_ptr_null(imported);

    /* Reload and confirm the ORIGINAL row survived untouched. */
    Session *survivor = session_manager_load_session_alloc(sm, "DUP-ID");
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
    Session *fresh = session_manager_import_session_new(sm, json2);
    ck_assert_ptr_nonnull(fresh);
    ck_assert_str_eq(fresh->id, "UNIQUE-ID");
    session_free(fresh);

    Session *loaded2 = session_manager_load_session_alloc(sm, "UNIQUE-ID");
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

    SessionManager *sm = session_manager_create(data_dir, "test-password");
    ck_assert_ptr_nonnull(sm);
    session_manager_free(sm);

    char salt_path[8192];
    snprintf(salt_path, sizeof(salt_path), "%s/salt", data_dir);
    ck_assert_int_eq(access(salt_path, F_OK), 0);
}
END_TEST

START_TEST(test_recovery_restores_backup_when_new_salt_was_not_created)
{
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

}
END_TEST

START_TEST(test_first_run_verifier_failure_removes_orphan_salt)
{
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

}
END_TEST

START_TEST(test_provider_state_survives_encrypted_reload_and_retained_owner)
{
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
    Session *loaded = session_manager_load_session_alloc(retained, session_id);
    ck_assert_ptr_nonnull(loaded);
    ck_assert_int_eq(loaded->messages_count, 1);
    ck_assert_str_eq(loaded->messages[0].provider_state,
        "[{\"type\":\"reasoning\",\"encrypted_content\":\"cipher\"}]");
    ck_assert_str_eq(loaded->messages[0].phase, "commentary");
    session_free(loaded);
    free(session_id);
    session_manager_free(retained);

}
END_TEST


Suite *session_mgr_crud_suite(void)
{
    Suite *s = suite_create("SessionManagerCrud");

    TCase *tc = tcase_create("CRUD");
    tcase_add_checked_fixture(tc, tmpdir_setup, tmpdir_teardown);
    tcase_set_timeout(tc, 180);
    tcase_add_test(tc, test_user_memory_table_ready_after_sm_create);
    tcase_add_test(tc, test_save_session_bind_failure_aborts);
    tcase_add_test(tc, test_purge_sessions_bind_failure_returns_error);
    tcase_add_test(tc, test_purge_sessions_rejects_bad_days);
    tcase_add_test(tc, test_delete_session_distinguishes_missing);
    tcase_add_test(tc, test_empty_or_null_id_refused);
    tcase_add_test(tc, test_session_list_alloc_fail_mid);
    tcase_add_test(tc, test_session_list_realloc_failures_are_safe);
    tcase_add_test(tc, test_encryption_rejects_hmac_valid_bad_padding_token);
    tcase_add_test(tc, test_encryption_rejects_future_timestamp_token);
    tcase_add_test(tc, test_first_run_rejects_short_password);
    tcase_add_test(tc, test_vault_without_pepper_cannot_unlock);
    tcase_add_test(tc, test_key_material_file_modes);
    tcase_add_test(tc, test_key_derive_pepper_changes_key);
    tcase_add_test(tc, test_import_rejects_duplicate_id_preserves_existing);
    tcase_add_test(tc, test_create_creates_missing_parent_dirs);
    tcase_add_test(tc, test_recovery_restores_backup_when_new_salt_was_not_created);
    tcase_add_test(tc, test_first_run_verifier_failure_removes_orphan_salt);
    tcase_add_test(tc, test_provider_state_survives_encrypted_reload_and_retained_owner);

    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    Suite *s = session_mgr_crud_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
