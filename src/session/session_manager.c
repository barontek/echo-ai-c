#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include <sqlite3.h>

#include "session_manager.h"
#include "migration.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

#define SALT_FILE "salt"

static int mkdir_p(const char *path)
{
    char tmp[1024];
    int len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len <= 0 || len >= (int)sizeof(tmp)) return -1;

    for (int i = 0; i < len; i++)
    {
        if (tmp[i] == '/')
        {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    return mkdir(path, 0755);
}

static int init_encryption(SessionManager *sm, const char *password)
{
    migration_check_and_recover(sm);

    char *salt_path = NULL;
    if (asprintf(&salt_path, "%s/%s", sm->data_dir, SALT_FILE) < 0) return -1;

    int is_first_run = encryption_first_run_detect(sm->data_dir);

    if (is_first_run)
    {
        log_info("first run detected, creating salt", NULL);
        if (encryption_salt_create(salt_path) != 0)
        {
            log_error("failed to create salt", NULL);
            free(salt_path);
            return -1;
        }
    }

    unsigned char salt[64];
    int salt_len = 0;
    if (encryption_salt_load(salt_path, salt, &salt_len) != 0)
    {
        log_error("failed to load salt", NULL);
        free(salt_path);
        return -1;
    }
    free(salt_path);

    if (encryption_key_derive(password, salt, salt_len, &sm->enc_key) != 0)
    {
        log_error("key derivation failed", NULL);
        return -1;
    }

    sm->key_initialized = 1;

    char *verifier_path = NULL;
    if (asprintf(&verifier_path, "%s/.verifier", sm->data_dir) >= 0)
    {
        struct stat st;
        int verifier_exists = (stat(verifier_path, &st) == 0);
        if (!verifier_exists)
        {
            if (encryption_create_verifier(&sm->enc_key, verifier_path) != 0)
                log_warn("failed to create verifier", NULL);
        }
        free(verifier_path);
    }

    log_info("encryption initialized", NULL);
    return 0;
}

SessionManager *session_manager_create(const char *data_dir, const char *password)
{
    if (!data_dir || !password) return NULL;

    SessionManager *sm = calloc(1, sizeof(SessionManager));
    if (!sm) return NULL;

    sm->data_dir = str_dup(data_dir);
    if (!sm->data_dir) { free(sm); return NULL; }

    if (init_encryption(sm, password) != 0)
    {
        log_error("failed to initialize encryption", NULL);
        session_manager_free(sm);
        return NULL;
    }

    if (session_manager_ensure_dirs(sm) != 0)
    {
        log_error("failed to ensure session directories", NULL);
        session_manager_free(sm);
        return NULL;
    }

    return sm;
}

void session_manager_free(SessionManager *sm)
{
    if (!sm) return;
    free(sm->data_dir);
    memset(sm, 0, sizeof(SessionManager));
    free(sm);
}

int session_manager_ensure_dirs(SessionManager *sm)
{
    if (!sm || !sm->data_dir) return -1;

    char *sessions_dir = NULL;
    if (asprintf(&sessions_dir, "%s/sessions", sm->data_dir) < 0) return -1;

    int rc = mkdir_p(sessions_dir);
    if (rc != 0 && errno != EEXIST)
    {
        log_error("failed to create sessions dir", "path", sessions_dir, NULL);
        free(sessions_dir);
        return -1;
    }

    free(sessions_dir);
    return 0;
}

static char *session_file_path(const char *data_dir, const char *id)
{
    char *path = NULL;
    if (asprintf(&path, "%s/sessions/%s.session", data_dir, id) < 0)
        return NULL;
    return path;
}

static int create_session_table(sqlite3 *db)
{
    const char *sql = "CREATE TABLE IF NOT EXISTS session ("
                      "id TEXT PRIMARY KEY,"
                      "title TEXT,"
                      "created_at TEXT,"
                      "messages_encrypted BLOB,"
                      "metadata_encrypted BLOB"
                      ");";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK)
    {
        log_error("sqlite create table", "err", err, NULL);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

Session *session_manager_create_session(SessionManager *sm, const char *title)
{
    if (!sm || !sm->key_initialized) return NULL;

    Session *s = session_create(title);
    if (!s) return NULL;

    if (session_manager_save_session(sm, s) != 0)
    {
        session_free(s);
        return NULL;
    }

    return s;
}

Session *session_manager_load_session(SessionManager *sm, const char *id)
{
    if (!sm || !id || !sm->key_initialized) return NULL;

    char *path = session_file_path(sm->data_dir, id);
    if (!path) return NULL;

    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK)
    {
        log_error("sqlite open failed", "path", path, NULL);
        free(path);
        return NULL;
    }
    free(path);

    Session *s = calloc(1, sizeof(Session));
    if (!s) { sqlite3_close(db); return NULL; }

    s->metadata = cJSON_CreateObject();
    s->events = NULL;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, title, created_at, messages_encrypted, metadata_encrypted "
                      "FROM session WHERE id = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("sqlite prepare", "err", sqlite3_errmsg(db), NULL);
        sqlite3_close(db);
        session_free(s);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        session_free(s);
        return NULL;
    }

    const char *db_id = (const char *)sqlite3_column_text(stmt, 0);
    const char *db_title = (const char *)sqlite3_column_text(stmt, 1);
    const char *db_created = (const char *)sqlite3_column_text(stmt, 2);
    const void *msgs_blob = sqlite3_column_blob(stmt, 3);
    int msgs_len = sqlite3_column_bytes(stmt, 3);
    const void *meta_blob = sqlite3_column_blob(stmt, 4);
    int meta_len = sqlite3_column_bytes(stmt, 4);

    s->id = str_dup(db_id ? db_id : "");
    s->title = str_dup(db_title ? db_title : "");
    s->created_at = str_dup(db_created ? db_created : "");

    if (msgs_blob && msgs_len > 0 && sm->key_initialized)
    {
        int dec_len = 0;
        unsigned char *dec = encryption_decrypt(&sm->enc_key, msgs_blob, msgs_len, &dec_len);
        if (dec)
        {
            session_deserialize_messages(s, (const char *)dec);
            free(dec);
        }
    }

    if (meta_blob && meta_len > 0 && sm->key_initialized)
    {
        int dec_len = 0;
        unsigned char *dec = encryption_decrypt(&sm->enc_key, meta_blob, meta_len, &dec_len);
        if (dec)
        {
            session_deserialize_metadata(s, (const char *)dec);
            free(dec);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return s;
}

int session_manager_save_session(SessionManager *sm, Session *session)
{
    if (!sm || !session || !sm->key_initialized) return -1;

    char *path = session_file_path(sm->data_dir, session->id);
    if (!path) return -1;

    int is_new = 0;
    struct stat st;
    if (stat(path, &st) != 0) is_new = 1;

    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK)
    {
        log_error("sqlite open for save", "path", path, NULL);
        free(path);
        return -1;
    }
    free(path);

    if (is_new)
    {
        if (create_session_table(db) != 0)
        {
            sqlite3_close(db);
            return -1;
        }
    }

    char *messages_json = session_serialize_messages(session);
    char *metadata_json = session_serialize_metadata(session);

    unsigned char *msgs_enc = NULL;
    int msgs_enc_len = 0;
    unsigned char *meta_enc = NULL;
    int meta_enc_len = 0;

    if (messages_json)
    {
        msgs_enc = encryption_encrypt(&sm->enc_key,
                                       (const unsigned char *)messages_json,
                                       strlen(messages_json), &msgs_enc_len);
    }

    if (metadata_json)
    {
        meta_enc = encryption_encrypt(&sm->enc_key,
                                       (const unsigned char *)metadata_json,
                                       strlen(metadata_json), &meta_enc_len);
    }

    const char *sql = "INSERT OR REPLACE INTO session (id, title, created_at, messages_encrypted, metadata_encrypted) "
                      "VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("sqlite prepare save", "err", sqlite3_errmsg(db), NULL);
        sqlite3_close(db);
        free(messages_json);
        free(metadata_json);
        free(msgs_enc);
        free(meta_enc);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, session->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session->title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, session->created_at, -1, SQLITE_TRANSIENT);
    if (msgs_enc && msgs_enc_len > 0)
        sqlite3_bind_blob(stmt, 4, msgs_enc, msgs_enc_len, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 4);
    if (meta_enc && meta_enc_len > 0)
        sqlite3_bind_blob(stmt, 5, meta_enc, meta_enc_len, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 5);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
    {
        log_error("sqlite step save", "err", sqlite3_errmsg(db), NULL);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        free(messages_json);
        free(metadata_json);
        free(msgs_enc);
        free(meta_enc);
        return -1;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    free(messages_json);
    free(metadata_json);
    free(msgs_enc);
    free(meta_enc);
    return 0;
}

int session_manager_delete_session(SessionManager *sm, const char *id)
{
    if (!sm || !id) return -1;

    char *path = session_file_path(sm->data_dir, id);
    if (!path) return -1;

    int rc = unlink(path);
    free(path);
    return rc == 0 ? 0 : -1;
}

SessionList *session_manager_list_sessions(SessionManager *sm)
{
    if (!sm || !sm->data_dir) return NULL;

    char *sessions_dir = NULL;
    if (asprintf(&sessions_dir, "%s/sessions", sm->data_dir) < 0) return NULL;

    DIR *dir = opendir(sessions_dir);
    if (!dir) { free(sessions_dir); return NULL; }

    SessionList *list = calloc(1, sizeof(SessionList));
    if (!list) { closedir(dir); free(sessions_dir); return NULL; }

    int capacity = 16;
    list->ids = malloc(sizeof(char *) * capacity);
    list->titles = malloc(sizeof(char *) * capacity);
    list->created_ats = malloc(sizeof(char *) * capacity);
    if (!list->ids || !list->titles || !list->created_ats)
    {
        closedir(dir);
        session_list_free(list);
        free(sessions_dir);
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (!str_ends_with(entry->d_name, ".session")) continue;

        char *dot = strrchr(entry->d_name, '.');
        if (!dot) continue;
        *dot = '\0';
        char *id = str_dup(entry->d_name);
        *dot = '.';
        if (!id) continue;

        Session *s = session_manager_load_session(sm, id);
        if (!s) { free(id); continue; }

        if (list->count >= capacity)
        {
            capacity *= 2;
            char **new_ids = realloc(list->ids, sizeof(char *) * capacity);
            char **new_titles = realloc(list->titles, sizeof(char *) * capacity);
            char **new_dates = realloc(list->created_ats, sizeof(char *) * capacity);
            if (!new_ids || !new_titles || !new_dates)
            {
                free(id);
                session_free(s);
                break;
            }
            list->ids = new_ids;
            list->titles = new_titles;
            list->created_ats = new_dates;
        }

        list->ids[list->count] = s->id;
        list->titles[list->count] = s->title;
        list->created_ats[list->count] = s->created_at;
        s->id = NULL;
        s->title = NULL;
        s->created_at = NULL;
        list->count++;
        session_free(s);
    }

    closedir(dir);
    free(sessions_dir);
    return list;
}

void session_list_free(SessionList *list)
{
    if (!list) return;
    for (int i = 0; i < list->count; i++)
    {
        free(list->ids[i]);
        free(list->titles[i]);
        free(list->created_ats[i]);
    }
    free(list->ids);
    free(list->titles);
    free(list->created_ats);
    free(list);
}
