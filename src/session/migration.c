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

    /* re-encrypt all sessions */
    SessionList *list = session_manager_list_sessions(sm);
    if (list)
    {
        EncryptionKey old_key = sm->enc_key;
        sm->enc_key = new_key;

        for (int i = 0; i < list->count; i++)
        {
            Session *s = session_manager_load_session(sm, list->ids[i]);
            if (!s) continue;

            if (session_manager_save_session(sm, s) != 0)
                log_warn("failed to re-encrypt session", "id", list->ids[i], NULL);

            session_free(s);
        }

        sm->enc_key = old_key;
        session_list_free(list);
    }

    sm->enc_key = new_key;

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
