#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "migration.h"
#include "encryption.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

#define MARKER_FILE ".changing_pwd"
#define SALT_FILE "salt"

static char *marker_path(const char *data_dir)
{
    char *path = NULL;
    if (asprintf(&path, "%s/%s", data_dir, MARKER_FILE) < 0) return NULL;
    return path;
}

static char *salt_path(const char *data_dir)
{
    char *path = NULL;
    if (asprintf(&path, "%s/%s", data_dir, SALT_FILE) < 0) return NULL;
    return path;
}

int migration_check_and_recover(SessionManager *sm)
{
    if (!sm || !sm->data_dir) return 0;

    char *mp = marker_path(sm->data_dir);
    if (!mp) return -1;

    struct stat st;
    int marker_exists = (stat(mp, &st) == 0);
    free(mp);

    if (!marker_exists) return 0;

    log_info("crash marker found, recovering", NULL);

    char *sp = salt_path(sm->data_dir);
    if (!sp) return -1;

    char *old_sp = NULL;
    if (asprintf(&old_sp, "%s.old", sp) < 0) { free(sp); return -1; }

    if (stat(old_sp, &st) == 0)
    {
        if (rename(old_sp, sp) != 0)
        {
            log_error("failed to restore old salt", NULL);
            free(sp); free(old_sp);
            return -1;
        }
        log_info("restored old salt", NULL);
    }

    free(old_sp);
    free(sp);

    mp = marker_path(sm->data_dir);
    if (mp) { unlink(mp); free(mp); }

    log_info("crash recovery complete", NULL);
    return 0;
}

int migration_change_password(SessionManager *sm, const char *new_password)
{
    if (!sm || !new_password || !sm->data_dir) return -1;

    char *sp = salt_path(sm->data_dir);
    if (!sp) return -1;

    char *old_sp = NULL;
    if (asprintf(&old_sp, "%s.old", sp) < 0) { free(sp); return -1; }

    char *mp = marker_path(sm->data_dir);
    if (!mp) { free(sp); free(old_sp); return -1; }

    /* save old salt */
    if (rename(sp, old_sp) != 0)
    {
        log_error("failed to backup old salt", NULL);
        free(sp); free(old_sp); free(mp);
        return -1;
    }

    /* create marker */
    FILE *f = fopen(mp, "wbx");
    if (!f)
    {
        log_error("failed to create marker", NULL);
        rename(old_sp, sp);
        free(sp); free(old_sp); free(mp);
        return -1;
    }
    fclose(f);

    /* create new salt */
    if (encryption_salt_create(sp) != 0)
    {
        log_error("failed to create new salt", NULL);
        unlink(mp);
        rename(old_sp, sp);
        free(sp); free(old_sp); free(mp);
        return -1;
    }

    /* derive new key */
    unsigned char new_salt[64];
    int new_salt_len = 0;
    if (encryption_salt_load(sp, new_salt, &new_salt_len) != 0)
    {
        log_error("failed to load new salt", NULL);
        unlink(mp);
        rename(old_sp, sp);
        free(sp); free(old_sp); free(mp);
        return -1;
    }

    EncryptionKey new_key;
    if (encryption_key_derive(new_password, new_salt, new_salt_len, &new_key) != 0)
    {
        log_error("key derivation failed", NULL);
        unlink(mp);
        rename(old_sp, sp);
        free(sp); free(old_sp); free(mp);
        return -1;
    }

    /* E1: re-encrypt all sessions under the held lock inside a single
     * SQLite transaction so a crash mid-loop rolls back atomically.
     * Previously each per-row load/save was a separate mutex grab+release,
     * and a crash left half the rows re-encrypted with the new key and
     * half with the old — permanent data loss. Also a concurrent
     * load/save from another thread could observe the transient enckey
     * swap and mis-encrypt. */

    EncryptionKey old_key = sm->enc_key;

    /* E1: hold sm->lock for the full transaction to isolate the key swap */
    session_manager_lock(sm);

    char *exec_err = NULL;
    if (sqlite3_exec(sm->db, "BEGIN IMMEDIATE", NULL, NULL, &exec_err) != SQLITE_OK)
    {
        log_error("migration: BEGIN IMMEDIATE failed",
                  "err", exec_err ? exec_err : "?", NULL);
        sqlite3_free(exec_err);
        session_manager_unlock(sm);
        /* revert marker + salt */
        unlink(mp);
        rename(old_sp, sp);
        free(sp); free(old_sp); free(mp);
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    const char *id_sql = "SELECT id FROM agent_sessions ORDER BY created_at";
    int ok = (sqlite3_prepare_v2(sm->db, id_sql, -1, &stmt, NULL) == SQLITE_OK);
    if (!ok)
    {
        log_error("migration: prepare SELECT id failed",
                  "err", sqlite3_errmsg(sm->db), NULL);
        sqlite3_exec(sm->db, "ROLLBACK", NULL, NULL, NULL);
        sm->enc_key = old_key;
        session_manager_unlock(sm);
        unlink(mp); rename(old_sp, sp);
        free(sp); free(old_sp); free(mp);
        return -1;
    }

    int migration_failed = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *sid = (const char *)sqlite3_column_text(stmt, 0);
        if (!sid) continue;

        /* Load with old key, save with new key.
         * sm->enc_key swap is race-free only because sm->lock is held. */
        sm->enc_key = old_key;
        Session *s = session_manager_load_session_nolock(sm, sid);
        if (!s) continue;

        sm->enc_key = new_key;
        if (session_manager_save_session_nolock(sm, s) != 0)
        {
            log_error("migration: re-encrypt failed, rolling back",
                      "id", sid, NULL);
            session_free(s);
            migration_failed = 1;
            break;
        }
        session_free(s);
    }
    sqlite3_finalize(stmt);

    if (migration_failed)
    {
        sqlite3_exec(sm->db, "ROLLBACK", NULL, NULL, NULL);
        /* E1 specific instruction: restore old key BEFORE unlock so the
         * next waiter sees the key that matches the DB content (which
         * was rolled back to old-key-encrypted). */
        sm->enc_key = old_key;
        session_manager_unlock(sm);
        /* marker serves as "migration not complete" signal for crash
         * recovery; since we rolled back in-process, clean it up. */
        unlink(mp);
        rename(old_sp, sp);
        free(sp); free(old_sp); free(mp);
        return -1;
    }

    if (sqlite3_exec(sm->db, "COMMIT", NULL, NULL, &exec_err) != SQLITE_OK)
    {
        log_error("migration: COMMIT failed",
                  "err", exec_err ? exec_err : "?", NULL);
        sqlite3_free(exec_err);
        sqlite3_exec(sm->db, "ROLLBACK", NULL, NULL, NULL);
        sm->enc_key = old_key;
        session_manager_unlock(sm);
        unlink(mp); rename(old_sp, sp);
        free(sp); free(old_sp); free(mp);
        return -1;
    }

    /* E1: only on COMMIT success do we make the new key permanent */
    sm->enc_key = new_key;
    session_manager_unlock(sm);

    /* create new verifier */
    char *verifier_path = NULL;
    if (asprintf(&verifier_path, "%s/.verifier", sm->data_dir) >= 0)
    {
        unlink(verifier_path);
        encryption_create_verifier(&sm->enc_key, verifier_path);
        free(verifier_path);
    }

    /* remove marker + old salt */
    unlink(mp);
    unlink(old_sp);

    free(sp); free(old_sp); free(mp);

    log_info("password changed successfully", NULL);
    return 0;
}
