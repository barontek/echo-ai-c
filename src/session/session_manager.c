#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include <sqlite3.h>

#include "session_manager.h"
#include "migration.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

#define SALT_FILE "salt"
#define DB_FILE "echo-ai.db"

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

static int init_db(sqlite3 *db)
{
    const char *sql = "CREATE TABLE IF NOT EXISTS agent_sessions ("
                      "id TEXT PRIMARY KEY,"
                      "title TEXT,"
                      "title_generation_attempted INTEGER DEFAULT 0,"
                      "created_at TEXT,"
                      "messages_encrypted BLOB,"
                      "metadata_encrypted BLOB,"
                      "events_encrypted BLOB"
                      ");";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK)
    {
        log_error("sqlite create agent_sessions table", "err", err, NULL);
        sqlite3_free(err);
        return -1;
    }

    sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=FULL", NULL, NULL, NULL);

    return 0;
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

    pthread_mutex_init(&sm->lock, NULL);

    if (init_encryption(sm, password) != 0)
    {
        log_error("failed to initialize encryption", NULL);
        session_manager_free(sm);
        return NULL;
    }

    mkdir_p(data_dir);

    char *db_path = NULL;
    if (asprintf(&db_path, "%s/%s", sm->data_dir, DB_FILE) < 0)
    {
        session_manager_free(sm);
        return NULL;
    }

    if (sqlite3_open(db_path, &sm->db) != SQLITE_OK)
    {
        log_error("failed to open session database", "path", db_path, NULL);
        free(db_path);
        session_manager_free(sm);
        return NULL;
    }
    free(db_path);

    if (init_db(sm->db) != 0)
    {
        session_manager_free(sm);
        return NULL;
    }

    log_info("session manager initialized", "data_dir", sm->data_dir, NULL);
    return sm;
}

void session_manager_free(SessionManager *sm)
{
    if (!sm) return;
    if (sm->db) sqlite3_close(sm->db);
    pthread_mutex_destroy(&sm->lock);
    free(sm->data_dir);
    memset(sm, 0, sizeof(SessionManager));
    free(sm);
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
    if (!sm || !id || !sm->key_initialized || !sm->db) return NULL;

    pthread_mutex_lock(&sm->lock);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, title, title_generation_attempted, created_at, "
                      "messages_encrypted, metadata_encrypted, events_encrypted "
                      "FROM agent_sessions WHERE id = ?";
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("sqlite prepare", "err", sqlite3_errmsg(sm->db), NULL);
        pthread_mutex_unlock(&sm->lock);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&sm->lock);
        return NULL;
    }

    Session *s = calloc(1, sizeof(Session));
    if (!s) { sqlite3_finalize(stmt); pthread_mutex_unlock(&sm->lock); return NULL; }

    const char *db_id = (const char *)sqlite3_column_text(stmt, 0);
    const char *db_title = (const char *)sqlite3_column_text(stmt, 1);
    int db_tga = sqlite3_column_int(stmt, 2);
    const char *db_created = (const char *)sqlite3_column_text(stmt, 3);
    const void *msgs_blob = sqlite3_column_blob(stmt, 4);
    int msgs_len = sqlite3_column_bytes(stmt, 4);
    const void *meta_blob = sqlite3_column_blob(stmt, 5);
    int meta_len = sqlite3_column_bytes(stmt, 5);
    const void *events_blob = sqlite3_column_blob(stmt, 6);
    int events_len = sqlite3_column_bytes(stmt, 6);

    s->id = str_dup(db_id ? db_id : "");
    s->title = str_dup(db_title ? db_title : "");
    s->title_generation_attempted = db_tga;
    s->created_at = str_dup(db_created ? db_created : "");
    s->events = cJSON_CreateArray();
    s->metadata = cJSON_CreateObject();

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

    if (events_blob && events_len > 0 && sm->key_initialized)
    {
        int dec_len = 0;
        unsigned char *dec = encryption_decrypt(&sm->enc_key, events_blob, events_len, &dec_len);
        if (dec)
        {
            session_deserialize_events(s, (const char *)dec);
            free(dec);
        }
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&sm->lock);
    return s;
}

int session_manager_save_session(SessionManager *sm, Session *session)
{
    if (!sm || !session || !sm->key_initialized || !sm->db) return -1;

    char *messages_json = session_serialize_messages(session);
    char *metadata_json = session_serialize_metadata(session);
    char *events_json = session_serialize_events(session);

    unsigned char *msgs_enc = NULL;
    int msgs_enc_len = 0;
    unsigned char *meta_enc = NULL;
    int meta_enc_len = 0;
    unsigned char *events_enc = NULL;
    int events_enc_len = 0;

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

    if (events_json)
    {
        events_enc = encryption_encrypt(&sm->enc_key,
                                         (const unsigned char *)events_json,
                                         strlen(events_json), &events_enc_len);
    }

    pthread_mutex_lock(&sm->lock);

    const char *sql = "INSERT OR REPLACE INTO agent_sessions "
                      "(id, title, title_generation_attempted, created_at, "
                      "messages_encrypted, metadata_encrypted, events_encrypted) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("sqlite prepare save", "err", sqlite3_errmsg(sm->db), NULL);
        pthread_mutex_unlock(&sm->lock);
        free(messages_json);
        free(metadata_json);
        free(events_json);
        free(msgs_enc);
        free(meta_enc);
        free(events_enc);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, session->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session->title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, session->title_generation_attempted);
    sqlite3_bind_text(stmt, 4, session->created_at, -1, SQLITE_TRANSIENT);
    if (msgs_enc && msgs_enc_len > 0)
        sqlite3_bind_blob(stmt, 5, msgs_enc, msgs_enc_len, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 5);
    if (meta_enc && meta_enc_len > 0)
        sqlite3_bind_blob(stmt, 6, meta_enc, meta_enc_len, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 6);
    if (events_enc && events_enc_len > 0)
        sqlite3_bind_blob(stmt, 7, events_enc, events_enc_len, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 7);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
    {
        log_error("sqlite step save", "err", sqlite3_errmsg(sm->db), NULL);
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&sm->lock);
        free(messages_json);
        free(metadata_json);
        free(events_json);
        free(msgs_enc);
        free(meta_enc);
        free(events_enc);
        return -1;
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&sm->lock);

    free(messages_json);
    free(metadata_json);
    free(events_json);
    free(msgs_enc);
    free(meta_enc);
    free(events_enc);
    return 0;
}

int session_manager_delete_session(SessionManager *sm, const char *id)
{
    if (!sm || !id || !sm->db) return -1;

    pthread_mutex_lock(&sm->lock);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "DELETE FROM agent_sessions WHERE id = ?";
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("sqlite prepare delete", "err", sqlite3_errmsg(sm->db), NULL);
        pthread_mutex_unlock(&sm->lock);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&sm->lock);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

SessionList *session_manager_list_sessions(SessionManager *sm)
{
    if (!sm || !sm->data_dir || !sm->db) return NULL;

    pthread_mutex_lock(&sm->lock);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, title, title_generation_attempted, created_at "
                      "FROM agent_sessions ORDER BY created_at DESC";
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("sqlite prepare list", "err", sqlite3_errmsg(sm->db), NULL);
        pthread_mutex_unlock(&sm->lock);
        return NULL;
    }

    SessionList *list = calloc(1, sizeof(SessionList));
    if (!list) { sqlite3_finalize(stmt); pthread_mutex_unlock(&sm->lock); return NULL; }

    int capacity = 16;
    list->ids = malloc(sizeof(char *) * capacity);
    list->titles = malloc(sizeof(char *) * capacity);
    list->created_ats = malloc(sizeof(char *) * capacity);
    list->title_generation_attempteds = malloc(sizeof(int) * capacity);
    if (!list->ids || !list->titles || !list->created_ats || !list->title_generation_attempteds)
    {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&sm->lock);
        session_list_free(list);
        return NULL;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        if (list->count >= capacity)
        {
            capacity *= 2;
            char **new_ids = realloc(list->ids, sizeof(char *) * capacity);
            char **new_titles = realloc(list->titles, sizeof(char *) * capacity);
            char **new_dates = realloc(list->created_ats, sizeof(char *) * capacity);
            int *new_tgas = realloc(list->title_generation_attempteds, sizeof(int) * capacity);
            if (!new_ids || !new_titles || !new_dates || !new_tgas) break;
            list->ids = new_ids;
            list->titles = new_titles;
            list->created_ats = new_dates;
            list->title_generation_attempteds = new_tgas;
        }

        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        const char *title = (const char *)sqlite3_column_text(stmt, 1);
        int tga = sqlite3_column_int(stmt, 2);
        const char *created = (const char *)sqlite3_column_text(stmt, 3);

        list->ids[list->count] = str_dup(id ? id : "");
        list->titles[list->count] = str_dup(title ? title : "");
        list->created_ats[list->count] = str_dup(created ? created : "");
        list->title_generation_attempteds[list->count] = tga;
        list->count++;
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&sm->lock);
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
    free(list->title_generation_attempteds);
    free(list);
}

int session_manager_add_message(SessionManager *sm, const char *session_id,
                                 const char *role, const char *content,
                                 const char *tool_call_id, const char *tool_name)
{
    Session *s = session_manager_load_session(sm, session_id);
    if (!s) return -1;

    int idx = s->messages_count;
    int new_count = idx + 1;
    Message *new_msgs = realloc(s->messages, sizeof(Message) * new_count);
    if (!new_msgs) { session_free(s); return -1; }

    s->messages = new_msgs;
    s->messages_count = new_count;
    memset(&s->messages[idx], 0, sizeof(Message));
    s->messages[idx].role = str_dup(role ? role : "");
    s->messages[idx].content = str_dup(content ? content : "");
    if (tool_call_id) s->messages[idx].tool_call_id = str_dup(tool_call_id);
    if (tool_name) s->messages[idx].tool_name = str_dup(tool_name);

    int rc = session_manager_save_session(sm, s);
    session_free(s);
    return rc;
}

int session_manager_truncate_history(SessionManager *sm, const char *session_id, int index)
{
    Session *s = session_manager_load_session(sm, session_id);
    if (!s) return -1;

    if (index < 0 || index >= s->messages_count) { session_free(s); return -1; }

    for (int i = index; i < s->messages_count; i++)
    {
        free(s->messages[i].role);
        free(s->messages[i].content);
        free(s->messages[i].id);
        free(s->messages[i].tool_call_id);
        free(s->messages[i].tool_name);
        free(s->messages[i].error_category);
        free(s->messages[i].thinking);
        if (s->messages[i].tool_calls)
        {
            for (int j = 0; j < s->messages[i].tool_calls_count; j++)
                tool_call_free(&s->messages[i].tool_calls[j]);
            free(s->messages[i].tool_calls);
        }
    }
    s->messages_count = index;

    int rc = session_manager_save_session(sm, s);
    session_free(s);
    return rc;
}

char *session_manager_export_session(SessionManager *sm, const char *session_id)
{
    Session *s = session_manager_load_session(sm, session_id);
    if (!s) return NULL;

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "id", s->id ? s->id : "");
    cJSON_AddStringToObject(json, "title", s->title ? s->title : "");
    cJSON *tga = cJSON_CreateNumber(s->title_generation_attempted);
    cJSON_AddItemToObject(json, "title_generation_attempted", tga);
    cJSON_AddStringToObject(json, "created_at", s->created_at ? s->created_at : "");

    if (s->messages_count > 0)
    {
        cJSON *arr = messages_to_json_array(s->messages, s->messages_count);
        if (arr) cJSON_AddItemToObject(json, "messages", arr);
    }

    if (s->metadata)
    {
        cJSON *meta = cJSON_Duplicate(s->metadata, 1);
        if (meta) cJSON_AddItemToObject(json, "metadata", meta);
    }

    if (s->events)
    {
        cJSON *ev = cJSON_Duplicate(s->events, 1);
        if (ev) cJSON_AddItemToObject(json, "events", ev);
    }

    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    session_free(s);
    return str;
}

Session *session_manager_import_session(SessionManager *sm, const char *json_str)
{
    if (!sm || !json_str) return NULL;

    cJSON *json = cJSON_Parse(json_str);
    if (!json) return NULL;

    cJSON *id_item = cJSON_GetObjectItem(json, "id");
    if (id_item && id_item->valuestring)
    {
        Session *existing = session_manager_load_session(sm, id_item->valuestring);
        if (existing)
        {
            session_free(existing);
            cJSON_Delete(json);
            return NULL;
        }
    }

    Session *s = session_create("Imported Session");
    if (!s) { cJSON_Delete(json); return NULL; }

    if (id_item && id_item->valuestring)
    {
        free(s->id);
        s->id = str_dup(id_item->valuestring);
    }

    cJSON *title_item = cJSON_GetObjectItem(json, "title");
    if (title_item && title_item->valuestring)
    {
        free(s->title);
        s->title = str_dup(title_item->valuestring);
    }

    cJSON *tga_item = cJSON_GetObjectItem(json, "title_generation_attempted");
    if (tga_item && cJSON_IsNumber(tga_item))
        s->title_generation_attempted = tga_item->valueint;

    cJSON *created_item = cJSON_GetObjectItem(json, "created_at");
    if (created_item && created_item->valuestring)
    {
        free(s->created_at);
        s->created_at = str_dup(created_item->valuestring);
    }

    cJSON *msgs = cJSON_GetObjectItem(json, "messages");
    if (msgs && cJSON_IsArray(msgs))
    {
        char *msgs_str = cJSON_PrintUnformatted(msgs);
        if (msgs_str)
        {
            session_deserialize_messages(s, msgs_str);
            free(msgs_str);
        }
    }

    cJSON *meta = cJSON_GetObjectItem(json, "metadata");
    if (meta)
    {
        char *meta_str = cJSON_PrintUnformatted(meta);
        if (meta_str)
        {
            session_deserialize_metadata(s, meta_str);
            free(meta_str);
        }
    }

    cJSON *events = cJSON_GetObjectItem(json, "events");
    if (events && cJSON_IsArray(events))
    {
        char *ev_str = cJSON_PrintUnformatted(events);
        if (ev_str)
        {
            session_deserialize_events(s, ev_str);
            free(ev_str);
        }
    }

    cJSON_Delete(json);

    if (session_manager_save_session(sm, s) != 0)
    {
        session_free(s);
        return NULL;
    }

    return s;
}

int session_manager_purge_sessions(SessionManager *sm, int older_than_days)
{
    if (!sm || !sm->db) return -1;

    time_t cutoff = time(NULL) - (time_t)older_than_days * 86400;
    struct tm *tm_cutoff = localtime(&cutoff);
    if (!tm_cutoff) return -1;

    char cutoff_str[64];
    strftime(cutoff_str, sizeof(cutoff_str), "%Y-%m-%dT%H:%M:%S", tm_cutoff);

    pthread_mutex_lock(&sm->lock);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "DELETE FROM agent_sessions WHERE created_at < ?";
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        pthread_mutex_unlock(&sm->lock);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, cutoff_str, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int deleted = sqlite3_changes(sm->db);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&sm->lock);

    return deleted;
}

int session_manager_log_event(SessionManager *sm, const char *session_id,
                               const char *event_type, const char *data)
{
    Session *s = session_manager_load_session(sm, session_id);
    if (!s) return -1;

    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "event_type", event_type ? event_type : "");
    cJSON_AddStringToObject(ev, "data", data ? data : "");

    time_t now = time(NULL);
    char ts[64];
    struct tm *tm_ptr = localtime(&now);
    if (tm_ptr) strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm_ptr);
    else snprintf(ts, sizeof(ts), "%ld", (long)now);
    cJSON_AddStringToObject(ev, "timestamp", ts);

    cJSON_AddItemToArray(s->events, ev);

    int rc = session_manager_save_session(sm, s);
    session_free(s);
    return rc;
}
