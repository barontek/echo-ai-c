/*
 * migration.c - crash-safe password migration for the session store:
 * re-encrypts all owned rows under a new password and recovers an
 * interrupted migration on the next startup.
 * Depends on: sqlite3, session_manager, encryption, logging.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "migration.h"
#include "encryption.h"
#include "../utils/logging.h"

#define DB_FILE "echo-ai.db"
#define MARKER_FILE ".changing_pwd"
#define SALT_FILE "salt"
#define PEPPER_FILE ".pepper"
#define VERIFIER_FILE ".verifier"
#define VERIFIER_NEW_FILE ".verifier.new"

#ifdef SESSION_MANAGER_TEST
/* Fault-injection knob: lets a test force the Nth rename() call in this
 * file to fail, so the -2 verifier-activation path can be exercised.
 * Production builds never see this; only translation units compiled
 * with -DSESSION_MANAGER_TEST=1 do. The body is defined before the
 * #define so it calls the real rename(). */
static int mig_rename_counter = 0;
static int mig_rename_fail_at = -1;

void migration_test_set_rename_fail(int nth_rename)
{
    mig_rename_counter = 0;
    mig_rename_fail_at = nth_rename;
}

static int mig_test_rename(const char *old_path, const char *new_path)
{
    mig_rename_counter++;
    if (mig_rename_counter == mig_rename_fail_at) return -1;
    return rename(old_path, new_path);
}
#define rename mig_test_rename

/* asprintf interception shared with session_manager.c / session_branch.c
 * (same counter, one knob): the migration paths allocate file paths and
 * re-encrypted rows via asprintf, so their OOM handling needs the same
 * fault injection as the str_dup counter provides elsewhere. */
int sm_test_asprintf(char **strp, const char *fmt, ...);
#define asprintf sm_test_asprintf
#endif

static int restore_old_files(SessionManager *sm, const char *salt,
                             const char *old_salt, const char *marker,
                             const char *verifier_new);

static char *data_path(const char *data_dir, const char *name)
{
    char *path = NULL;
    if (asprintf(&path, "%s/%s", data_dir, name) < 0) return NULL;
    return path;
}

/* Load the vault pepper for a data dir into a caller buffer. Every key
 * derivation in this file runs over salt||pepper, so all recovery and
 * migration paths must carry the pepper. */
static int load_pepper(const char *data_dir, unsigned char *pepper,
                       int *pepper_len)
{
    char *path = data_path(data_dir, PEPPER_FILE);
    if (!path) return -1;
    int rc = encryption_pepper_load(path, pepper, pepper_len);
    free(path);
    return rc;
}

static char *path_suffix(const char *path, const char *suffix)
{
    char *result = NULL;
    if (asprintf(&result, "%s%s", path, suffix) < 0) return NULL;
    return result;
}

static int remove_if_exists(const char *path)
{
    if (unlink(path) == 0 || errno == ENOENT) return 0;
    return -1;
}

static int sync_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    int saved_errno = errno;
    if (close(fd) != 0 && rc == 0)
    {
        rc = -1;
        saved_errno = errno;
    }
    errno = saved_errno;
    return rc;
}

static int sync_directory(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    int saved_errno = errno;
    if (close(fd) != 0 && rc == 0)
    {
        rc = -1;
        saved_errno = errno;
    }
    errno = saved_errno;
    return rc;
}

static int create_marker(const char *path)
{
    static const char marker[] = "pending\n";
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;

    size_t written = 0;
    int rc = 0;
    while (written < sizeof(marker) - 1U)
    {
        ssize_t count = write(fd, marker + written,
                              sizeof(marker) - 1U - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0)
        {
            rc = -1;
            break;
        }
        written += (size_t)count;
    }
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    if (rc != 0) (void)remove_if_exists(path);
    return rc;
}

static int sqlite_exec_checked(sqlite3 *db, const char *sql,
                               const char *operation)
{
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK)
        log_error(operation, "err", error ? error : sqlite3_errmsg(db), NULL);
    sqlite3_free(error);
    return rc == SQLITE_OK ? 0 : -1;
}

static int database_has_committed_salt(const char *data_dir,
                                       const unsigned char *salt, int salt_len,
                                       int *committed, int *state_exists)
{
    *committed = 0;
    *state_exists = 0;
    char *db_path = data_path(data_dir, DB_FILE);
    if (!db_path) return -1;
    if (access(db_path, F_OK) != 0)
    {
        free(db_path);
        return errno == ENOENT ? 0 : -1;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    free(db_path);
    if (rc != SQLITE_OK)
    {
        if (db) sqlite3_close(db);
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    const char *table_sql =
        "SELECT 1 FROM sqlite_master WHERE type='table' "
        "AND name='password_migration_state'";
    rc = sqlite3_prepare_v2(db, table_sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int table_exists = (rc == SQLITE_ROW);
    *state_exists = table_exists;
    if (table_exists) rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) rc = SQLITE_ERROR;
    if (stmt && sqlite3_finalize(stmt) != SQLITE_OK) rc = SQLITE_ERROR;
    stmt = NULL;

    if (rc == SQLITE_DONE && table_exists)
    {
        rc = sqlite3_prepare_v2(
            db, "SELECT salt FROM password_migration_state WHERE id=1",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW && sqlite3_column_type(stmt, 0) == SQLITE_BLOB)
        {
            const void *stored = sqlite3_column_blob(stmt, 0);
            int stored_len = sqlite3_column_bytes(stmt, 0);
            if (stored && stored_len == salt_len &&
                memcmp(stored, salt, (size_t)salt_len) == 0)
                *committed = 1;
            rc = sqlite3_step(stmt);
        }
        if (rc != SQLITE_DONE) rc = SQLITE_ERROR;
        if (stmt && sqlite3_finalize(stmt) != SQLITE_OK) rc = SQLITE_ERROR;
    }

    if (sqlite3_close(db) != SQLITE_OK) rc = SQLITE_ERROR;
    return rc == SQLITE_DONE ? 0 : -1;
}

static int legacy_key_evidence(const char *data_dir, const char *salt_path,
                               const char *password, int *has_encrypted_rows,
                               int *database_match, int *verifier_match)
{
    *has_encrypted_rows = 0;
    *database_match = 0;
    *verifier_match = 0;
    unsigned char salt[64] = {0};
    int salt_len = 0;
    unsigned char pepper[64] = {0};
    int pepper_len = 0;
    EncryptionKey key = {{0}};
    if (encryption_salt_load(salt_path, salt, &salt_len) != 0 ||
        load_pepper(data_dir, pepper, &pepper_len) != 0 ||
        encryption_key_derive(password, salt, salt_len,
                              pepper, pepper_len, &key) != 0)
        return -1;

    char *verifier = data_path(data_dir, VERIFIER_FILE);
    char *db_path = data_path(data_dir, DB_FILE);
    if (!verifier || !db_path)
    {
        free(verifier); free(db_path);
        memset(salt, 0, sizeof(salt)); memset(&key, 0, sizeof(key));
        return -1;
    }
    *verifier_match = encryption_check_verifier(&key, verifier) == 0;

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc == SQLITE_OK)
    {
        const char *sql =
            "SELECT data FROM ("
            "SELECT title_encrypted AS data FROM agent_sessions WHERE length(title_encrypted)>0 "
            "UNION ALL SELECT messages_encrypted FROM agent_sessions WHERE length(messages_encrypted)>0 "
            "UNION ALL SELECT metadata_encrypted FROM agent_sessions WHERE length(metadata_encrypted)>0 "
            "UNION ALL SELECT events_encrypted FROM agent_sessions WHERE length(events_encrypted)>0"
            ") LIMIT 1";
        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW)
        {
            const void *blob = sqlite3_column_blob(stmt, 0);
            int blob_len = sqlite3_column_bytes(stmt, 0);
            *has_encrypted_rows = blob && blob_len > 0;
            int plain_len = 0;
            unsigned char *plain = *has_encrypted_rows ?
                encryption_decrypt(&key, blob, blob_len, &plain_len) : NULL;
            *database_match = plain != NULL;
            if (plain)
            {
                memset(plain, 0, (size_t)plain_len);
                free(plain);
            }
            rc = sqlite3_step(stmt);
        }
        if (rc != SQLITE_DONE) rc = SQLITE_ERROR;
        if (stmt && sqlite3_finalize(stmt) != SQLITE_OK) rc = SQLITE_ERROR;
    }
    if (db && sqlite3_close(db) != SQLITE_OK) rc = SQLITE_ERROR;
    free(verifier); free(db_path);
    memset(salt, 0, sizeof(salt)); memset(&key, 0, sizeof(key));
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Finalizes a migration recovery: promotes the recovered key material,
 * removes the marker, and syncs the data dir. All path strings are
 * caller-owned; this function frees them. Returns 0 on success, -1 on
 * any failure. */
static int migration_finalize_recovery(SessionManager *sm, int committed,
                                       char *marker, char *salt,
                                       char *old_salt, char *verifier_new)
{
    struct stat st = {0};
    int result = 0;
    if (committed)
    {
        char *verifier = data_path(sm->data_dir, VERIFIER_FILE);
        if (!verifier)
            result = -1;  // NOLINT(bugprone-branch-clone): shared error
                          // assignment; TODO(migration): branches differ,
                          // only the failure outcome is identical
        else if (stat(verifier_new, &st) == 0 &&
                 rename(verifier_new, verifier) != 0)
            result = -1;
        free(verifier);
        if (result == 0 && remove_if_exists(old_salt) != 0) result = -1;
    }
    else if (stat(old_salt, &st) == 0)
    {
        if (rename(old_salt, salt) != 0) result = -1;
        if (result == 0 && remove_if_exists(verifier_new) != 0) result = -1;
    }
    else if (stat(salt, &st) != 0)
    {  // NOLINT(bugprone-branch-clone): shared error outcome;
       // TODO(migration): branches differ in condition, only failure is same
        result = -1;
    }
    else if (remove_if_exists(verifier_new) != 0)
    {
        result = -1;
    }

    if (result == 0 && remove_if_exists(marker) != 0) result = -1;
    if (result == 0 && sync_directory(sm->data_dir) != 0) result = -1;
    free(marker); free(salt); free(old_salt); free(verifier_new);
    if (result == 0) log_info("password migration recovery complete", NULL);
    return result;
}

int migration_check_and_recover(SessionManager *sm, const char *password)
{
    if (!sm || !sm->data_dir || !password) return -1;

    char *marker = data_path(sm->data_dir, MARKER_FILE);
    char *salt = data_path(sm->data_dir, SALT_FILE);
    char *old_salt = salt ? path_suffix(salt, ".old") : NULL;
    char *verifier_new = data_path(sm->data_dir, VERIFIER_NEW_FILE);
    if (!marker || !salt || !old_salt || !verifier_new)
    {
        free(marker); free(salt); free(old_salt); free(verifier_new);
        return -1;
    }

    struct stat st = {0};
    if (stat(marker, &st) != 0)
    {
        int result = errno == ENOENT ? 0 : -1;
        free(marker); free(salt); free(old_salt); free(verifier_new);
        return result;
    }

    log_info("password migration marker found, recovering", NULL);
    if (stat(salt, &st) != 0)
    {
        if (errno != ENOENT || stat(old_salt, &st) != 0 ||
            restore_old_files(sm, salt, old_salt, marker, verifier_new) != 0)
        {
            free(marker); free(salt); free(old_salt); free(verifier_new);
            return -1;
        }
        free(marker); free(salt); free(old_salt); free(verifier_new);
        log_info("password migration recovery complete", NULL);
        return 0;
    }
    unsigned char current_salt[64] = {0};
    int current_salt_len = 0;
    int committed = 0;
    int state_exists = 0;
    if (encryption_salt_load(salt, current_salt, &current_salt_len) != 0 ||
        database_has_committed_salt(sm->data_dir, current_salt,
                                    current_salt_len, &committed,
                                    &state_exists) != 0)
    {
        free(marker); free(salt); free(old_salt); free(verifier_new);
        return -1;
    }

    if (!state_exists && stat(old_salt, &st) == 0)
    {
        int current_has_rows = 0;
        int current_db_match = 0;
        int current_verifier_match = 0;
        int old_has_rows = 0;
        int old_db_match = 0;
        int old_verifier_match = 0;
        if (legacy_key_evidence(sm->data_dir, salt, password,
                                &current_has_rows, &current_db_match,
                                &current_verifier_match) != 0 ||
            legacy_key_evidence(sm->data_dir, old_salt, password,
                                &old_has_rows, &old_db_match,
                                &old_verifier_match) != 0)
        {
            free(marker); free(salt); free(old_salt); free(verifier_new);
            return -1;
        }
        if (current_has_rows || old_has_rows)
        {
            if (current_db_match == old_db_match)
            {
                free(marker); free(salt); free(old_salt); free(verifier_new);
                return -1;
            }
            committed = current_db_match;
        }
        else if (current_verifier_match != old_verifier_match)
        {
            committed = current_verifier_match;
        }
        else
        {
            free(marker); free(salt); free(old_salt); free(verifier_new);
            return -1;
        }
    }

    if (committed && !state_exists && stat(verifier_new, &st) != 0)
    {
        if (errno != ENOENT)
        {
            free(marker); free(salt); free(old_salt); free(verifier_new);
            return -1;
        }
        EncryptionKey recovered_key = {{0}};
        unsigned char recovered_pepper[64] = {0};
        int recovered_pepper_len = 0;
        if (load_pepper(sm->data_dir, recovered_pepper,
                        &recovered_pepper_len) != 0 ||
            encryption_key_derive(password, current_salt, current_salt_len,
                                  recovered_pepper, recovered_pepper_len,
                                  &recovered_key) != 0 ||
            encryption_create_verifier(&recovered_key, verifier_new) != 0 ||
            sync_file(verifier_new) != 0 ||
            sync_directory(sm->data_dir) != 0)
        {
            memset(&recovered_key, 0, sizeof(recovered_key));
            memset(recovered_pepper, 0, sizeof(recovered_pepper));
            (void)remove_if_exists(verifier_new);
            free(marker); free(salt); free(old_salt); free(verifier_new);
            return -1;
        }
        memset(&recovered_key, 0, sizeof(recovered_key));
        memset(recovered_pepper, 0, sizeof(recovered_pepper));
    }

    memset(current_salt, 0, sizeof(current_salt));
    return migration_finalize_recovery(sm, committed, marker, salt,
                                       old_salt, verifier_new);
}

static int migrate_sessions(SessionManager *sm, const EncryptionKey *old_key,
                            const EncryptionKey *new_key)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        sm->db, "SELECT id FROM agent_sessions ORDER BY id", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    int result = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        if (sqlite3_column_type(stmt, 0) != SQLITE_TEXT)
        {
            result = -1;
            break;
        }
        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        int id_len = sqlite3_column_bytes(stmt, 0);
        if (!id || id_len <= 0 || memchr(id, '\0', (size_t)id_len))
        {
            result = -1;
            break;
        }

        sm->enc_key = *old_key;
        Session *session = session_manager_load_session_nolock_alloc(sm, id);
        if (!session || !session->id || !session->title ||
            !session->created_at || !session->events || !session->metadata)
        {
            session_free(session);
            result = -1;
            break;
        }
        sm->enc_key = *new_key;
        if (session_manager_save_session_nolock(sm, session) != 0)
            result = -1;
        session_free(session);
        if (result != 0) break;
    }
    if (rc != SQLITE_DONE) result = -1;
    if (sqlite3_finalize(stmt) != SQLITE_OK) result = -1;
    return result;
}

static int migrate_provider_oauth(SessionManager *sm,
                                  const EncryptionKey *old_key,
                                  const EncryptionKey *new_key)
{
    sqlite3_stmt *select_stmt = NULL;
    sqlite3_stmt *update_stmt = NULL;
    int rc = sqlite3_prepare_v2(
        sm->db,
        "SELECT provider, data_encrypted FROM provider_oauth ORDER BY provider",
        -1, &select_stmt, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(
            sm->db,
            "UPDATE provider_oauth SET data_encrypted=? WHERE provider=?",
            -1, &update_stmt, NULL);
    if (rc != SQLITE_OK)
    {
        if (select_stmt) sqlite3_finalize(select_stmt);
        if (update_stmt) sqlite3_finalize(update_stmt);
        return -1;
    }

    int result = 0;
    while ((rc = sqlite3_step(select_stmt)) == SQLITE_ROW)
    {
        const char *provider = NULL;
        const void *blob = NULL;
        int provider_len = 0;
        int blob_len = 0;
        if (sqlite3_column_type(select_stmt, 0) == SQLITE_TEXT)
        {
            provider = (const char *)sqlite3_column_text(select_stmt, 0);
            provider_len = sqlite3_column_bytes(select_stmt, 0);
        }
        if (sqlite3_column_type(select_stmt, 1) == SQLITE_BLOB)
        {
            blob = sqlite3_column_blob(select_stmt, 1);
            blob_len = sqlite3_column_bytes(select_stmt, 1);
        }
        if (!provider || provider_len <= 0 ||
            memchr(provider, '\0', (size_t)provider_len) || !blob || blob_len <= 0)
        {
            result = -1;
            break;
        }

        int plain_len = 0;
        unsigned char *plain = encryption_decrypt(old_key, blob, blob_len,
                                                   &plain_len);
        if (!plain || plain_len <= 0 || memchr(plain, '\0', (size_t)plain_len))
        {
            free(plain);
            result = -1;
            break;
        }
        int encrypted_len = 0;
        unsigned char *encrypted = encryption_encrypt(new_key, plain, plain_len,
                                                       &encrypted_len);
        memset(plain, 0, (size_t)plain_len);
        free(plain);
        if (!encrypted || encrypted_len <= 0)
        {
            free(encrypted);
            result = -1;
            break;
        }

        int update_rc = sqlite3_bind_blob(update_stmt, 1, encrypted,
                                          encrypted_len, SQLITE_TRANSIENT);
        if (update_rc == SQLITE_OK)
            update_rc = sqlite3_bind_text(update_stmt, 2, provider,
                                          provider_len, SQLITE_TRANSIENT);
        if (update_rc == SQLITE_OK) update_rc = sqlite3_step(update_stmt);
        memset(encrypted, 0, (size_t)encrypted_len);
        free(encrypted);
        if (update_rc != SQLITE_DONE || sqlite3_changes(sm->db) != 1)
        {
            result = -1;
            break;
        }
        if (sqlite3_reset(update_stmt) != SQLITE_OK ||
            sqlite3_clear_bindings(update_stmt) != SQLITE_OK)
        {
            result = -1;
            break;
        }
    }
    if (rc != SQLITE_DONE) result = -1;
    if (sqlite3_finalize(update_stmt) != SQLITE_OK) result = -1;
    if (sqlite3_finalize(select_stmt) != SQLITE_OK) result = -1;
    return result;
}

static int record_committed_salt(sqlite3 *db, const unsigned char *salt,
                                 int salt_len)
{
    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS password_migration_state ("
        "id INTEGER PRIMARY KEY CHECK(id=1), salt BLOB NOT NULL)";
    if (sqlite_exec_checked(db, create_sql,
                            "migration: create state table failed") != 0)
        return -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO password_migration_state(id, salt) VALUES(1, ?) "
        "ON CONFLICT(id) DO UPDATE SET salt=excluded.salt";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_blob(stmt, 1, salt, salt_len, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
        log_error("migration: record committed salt failed", "err",
                  sqlite3_errmsg(db), NULL);
    if (stmt && sqlite3_finalize(stmt) != SQLITE_OK) rc = SQLITE_ERROR;
    return rc == SQLITE_DONE ? 0 : -1;
}

static int restore_old_files(SessionManager *sm, const char *salt,
                             const char *old_salt, const char *marker,
                             const char *verifier_new)
{
    int result = 0;
    if (rename(old_salt, salt) != 0) result = -1;
    if (remove_if_exists(verifier_new) != 0) result = -1;
    if (result == 0 && remove_if_exists(marker) != 0) result = -1;
    if (result == 0 && sync_directory(sm->data_dir) != 0) result = -1;
    return result;
}

/* Performs the password migration core while the manager lock is held:
 * marker/salt dance, re-encryption of every session and OAuth record in a
 * transaction, verifier activation, and rollback/restore on any failure.
 * Returns 0 on success, -1 on failure (old key material restored), or
 * -2 when the DB committed but the verifier could not be activated and
 * the recovery retry also failed (the caller should ask for a restart). */
static int migration_perform_change(SessionManager *sm, const char *new_password,
                                    char *salt, char *old_salt, char *marker,
                                    char *verifier, char *verifier_new)
{
    int result = -1;
    int transaction_open = 0;
    int transaction_committed = 0;
    EncryptionKey old_key = {{0}};
    EncryptionKey new_key = {{0}};
    unsigned char new_salt[64] = {0};
    int new_salt_len = 0;
    unsigned char new_pepper[64] = {0};
    int new_pepper_len = 0;
    old_key = sm->enc_key;

    if (create_marker(marker) != 0 || sync_directory(sm->data_dir) != 0)
        goto cleanup;
    if (rename(salt, old_salt) != 0 || sync_directory(sm->data_dir) != 0)
        goto restore;
    if (encryption_salt_create(salt) != 0 || sync_file(salt) != 0 ||
        sync_directory(sm->data_dir) != 0)
        goto restore;
    if (encryption_salt_load(salt, new_salt, &new_salt_len) != 0 ||
        new_salt_len <= 0 ||
        load_pepper(sm->data_dir, new_pepper, &new_pepper_len) != 0 ||
        encryption_key_derive(new_password, new_salt, new_salt_len,
                              new_pepper, new_pepper_len, &new_key) != 0)
        goto restore;
    if (remove_if_exists(verifier_new) != 0 ||
        encryption_create_verifier(&new_key, verifier_new) != 0 ||
        sync_file(verifier_new) != 0)
        goto restore;

    if (sqlite_exec_checked(sm->db, "BEGIN IMMEDIATE",
                            "migration: begin failed") != 0)
        goto restore;
    transaction_open = 1;
    if (migrate_sessions(sm, &old_key, &new_key) != 0 ||
        migrate_provider_oauth(sm, &old_key, &new_key) != 0 ||
        record_committed_salt(sm->db, new_salt, new_salt_len) != 0)
        goto rollback;
    if (sqlite_exec_checked(sm->db, "COMMIT", "migration: commit failed") != 0)
        goto rollback;
    transaction_open = 0;
    transaction_committed = 1;

    sm->enc_key = new_key;
    if (rename(verifier_new, verifier) != 0 ||
        sync_directory(sm->data_dir) != 0)
    {
        /* DB is committed under the new key but the verifier swap did
         * not land. Try the same recovery a restart would run, right
         * now: it renames verifier_new over the verifier (or, when the
         * rename had already succeeded and only the dir sync failed,
         * just drops the artifacts). Only if that retry also fails do
         * we keep the marker and report -2 so the caller can ask for a
         * restart. */
        log_error("migration: verifier activation failed, retrying via recovery", NULL);
        if (migration_check_and_recover(sm, new_password) == 0)
        {
            result = 0;
            goto cleanup;
        }
        result = -2;
        goto cleanup;
    }

    if (remove_if_exists(old_salt) != 0 || remove_if_exists(marker) != 0 ||
        sync_directory(sm->data_dir) != 0)
        log_warn("password changed but migration artifacts remain", NULL);
    result = 0;
    goto cleanup;

rollback:
    if (transaction_open)
    {
        (void)sqlite_exec_checked(sm->db, "ROLLBACK",
                                  "migration: rollback failed");
        transaction_open = 0;
    }
    sm->enc_key = old_key;

restore:
    if (restore_old_files(sm, salt, old_salt, marker, verifier_new) != 0)
        log_error("migration: failed to restore old files", NULL);

cleanup:
    if (transaction_open)
        (void)sqlite_exec_checked(sm->db, "ROLLBACK",
                                  "migration: cleanup rollback failed");
    if (result != 0 && !transaction_committed) sm->enc_key = old_key;
    memset(&old_key, 0, sizeof(old_key));
    memset(&new_key, 0, sizeof(new_key));
    memset(new_salt, 0, sizeof(new_salt));
    memset(new_pepper, 0, sizeof(new_pepper));
    return result;
}


int migration_change_password(SessionManager *sm, const char *new_password)
{
    if (!sm || !sm->db || !sm->key_initialized || !new_password ||
        !sm->data_dir)
        return -1;

    char *salt = data_path(sm->data_dir, SALT_FILE);
    char *old_salt = salt ? path_suffix(salt, ".old") : NULL;
    char *marker = data_path(sm->data_dir, MARKER_FILE);
    char *verifier = data_path(sm->data_dir, VERIFIER_FILE);
    char *verifier_new = data_path(sm->data_dir, VERIFIER_NEW_FILE);
    if (!salt || !old_salt || !marker || !verifier || !verifier_new)
    {
        free(salt); free(old_salt); free(marker); free(verifier);
        free(verifier_new);
        return -1;
    }

    session_manager_lock(sm);
    int result = migration_perform_change(sm, new_password, salt, old_salt,
                                          marker, verifier, verifier_new);
    session_manager_unlock(sm);
    free(salt); free(old_salt); free(marker); free(verifier);
    free(verifier_new);
    if (result == 0) log_info("password changed successfully", NULL);
    return result;
}
