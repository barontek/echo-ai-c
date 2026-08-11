/*
 * session_serialize.c - session row serialization: the shared
 * load-with-decrypt and serialize+encrypt+save bodies, plus
 * export/import.
 * Depends on: sqlite3, cJSON, encryption, session, message.
 */

#define _GNU_SOURCE
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <sqlite3.h>

#include "session_serialize.h"
#include "session_manager_internal.h"
#include "session_manager.h"
#include "session.h"
#include "../agent/message.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

#ifdef SESSION_MANAGER_TEST
/* Shared fault-injection hooks (counters live in session_manager.c):
 * route this TU's str_dup, bind, and encrypt calls through the shims
 * so session_manager_test_set_* reaches the whole module. */
#define str_dup sm_test_strdup
#define sqlite3_bind_text sm_test_bind_text
#define sqlite3_bind_int sm_test_bind_int
#define sqlite3_bind_blob sm_test_bind_blob
#define sqlite3_bind_null sm_test_bind_null
#define encryption_encrypt sm_test_encrypt
#endif


Session *load_session_locked(SessionManager *sm, const char *id)
{
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, title_encrypted, title_generation_attempted, created_at, "
                      "messages_encrypted, metadata_encrypted, events_encrypted "
                      "FROM agent_sessions WHERE id = ?";
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("sqlite prepare", "err", sqlite3_errmsg(sm->db), NULL);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW)
    {
        if (rc != SQLITE_DONE)
            log_error("sqlite step load", "err", sqlite3_errmsg(sm->db), NULL);
        sqlite3_finalize(stmt);
        return NULL;
    }

    Session *s = calloc(1, sizeof(Session));
    if (!s) {
        log_error("calloc Session in load", NULL);
        sqlite3_finalize(stmt);
        return NULL;
    }

    const char *db_id = (const char *)sqlite3_column_text(stmt, 0);
    const void *title_blob = sqlite3_column_blob(stmt, 1);
    int title_len = sqlite3_column_bytes(stmt, 1);
    int db_tga = sqlite3_column_int(stmt, 2);
    const char *db_created = (const char *)sqlite3_column_text(stmt, 3);
    const void *msgs_blob = sqlite3_column_blob(stmt, 4);
    int msgs_len = sqlite3_column_bytes(stmt, 4);
    const void *meta_blob = sqlite3_column_blob(stmt, 5);
    int meta_len = sqlite3_column_bytes(stmt, 5);
    const void *events_blob = sqlite3_column_blob(stmt, 6);
    int events_len = sqlite3_column_bytes(stmt, 6);

    s->id = str_dup(db_id ? db_id : "");
    s->title = str_dup("");
    s->title_generation_attempted = db_tga;
    s->created_at = str_dup(db_created ? db_created : "");
    s->events = cJSON_CreateArray();
    s->metadata = cJSON_CreateObject();
    if (!s->id || !s->title || !s->created_at || !s->events || !s->metadata)
    {
        log_error("load_session: allocation failure", "id", id, NULL);
        session_free(s);
        sqlite3_finalize(stmt);
        return NULL;
    }

    int partial_fail = 0;

    if (title_blob && title_len > 0 && sm->key_initialized)
    {
        int dec_len = 0;
        unsigned char *dec = encryption_decrypt(&sm->enc_key, title_blob, title_len, &dec_len);
        if (dec)
        {
            char *title_dup = str_dup((const char *)dec);
            free(dec);
            if (!title_dup)
            {
                log_error("load_session: title allocation failure", "id", id, NULL);
                session_free(s);
                sqlite3_finalize(stmt);
                return NULL;
            }
            free(s->title);
            s->title = title_dup;
        }
        else
        {
            log_error("load_session: title could not be decrypted", "id", id, NULL);
            partial_fail = 1;
        }
    }

    if (msgs_blob && msgs_len > 0 && sm->key_initialized)
    {
        int dec_len = 0;
        unsigned char *dec = encryption_decrypt(&sm->enc_key, msgs_blob, msgs_len, &dec_len);
        if (dec)
        {
            int rc_d = session_deserialize_messages(s, (const char *)dec);
            if (rc_d != 0)
            {
                log_error("load_session: messages decrypted but failed to parse",
                          "id", id, NULL);
                partial_fail = 1;
            }
            free(dec);
        }
        else
        {
            log_error("load_session: messages could not be decrypted", "id", id, NULL);
            partial_fail = 1;
        }
    }

    if (meta_blob && meta_len > 0 && sm->key_initialized)
    {
        int dec_len = 0;
        unsigned char *dec = encryption_decrypt(&sm->enc_key, meta_blob, meta_len, &dec_len);
        if (dec)
        {
            int rc_d = session_deserialize_metadata(s, (const char *)dec);
            if (rc_d != 0)
            {
                log_error("load_session: metadata decrypted but failed to parse",
                          "id", id, NULL);
                partial_fail = 1;
            }
            free(dec);
        }
        else
        {
            log_error("load_session: metadata could not be decrypted", "id", id, NULL);
            partial_fail = 1;
        }
    }

    if (events_blob && events_len > 0 && sm->key_initialized)
    {
        int dec_len = 0;
        unsigned char *dec = encryption_decrypt(&sm->enc_key, events_blob, events_len, &dec_len);
        if (dec)
        {
            int rc_d = session_deserialize_events(s, (const char *)dec);
            if (rc_d != 0)
            {
                log_error("load_session: events decrypted but failed to parse",
                          "id", id, NULL);
                partial_fail = 1;
            }
            free(dec);
        }
        else
        {
            log_error("load_session: events could not be decrypted", "id", id, NULL);
            partial_fail = 1;
        }
    }

    sqlite3_finalize(stmt);

    if (partial_fail)
    {
        session_free(s);
        return NULL;
    }

    return s;
}

static int session_encrypt_all_fields(SessionManager *sm,
                                      const char *title,
                                      const char *messages_json,
                                      const char *metadata_json,
                                      const char *events_json,
                                      unsigned char **title_enc, int *title_enc_len,
                                      unsigned char **msgs_enc, int *msgs_enc_len,
                                      unsigned char **meta_enc, int *meta_enc_len,
                                      unsigned char **events_enc, int *events_enc_len)
{
    int abort_save = 0;
    if (!abort_save && title && title[0])
    {
        *title_enc = encryption_encrypt(&sm->enc_key,
                                        (const unsigned char *)title,
                                        (int)strlen(title), title_enc_len);
        if (!*title_enc)
        {
            log_error("save_session: encryption_encrypt failed for title", NULL);
            abort_save = 1;
        }
    }

    if (!abort_save && messages_json)
    {
        *msgs_enc = encryption_encrypt(&sm->enc_key,
                                        (const unsigned char *)messages_json,
                                        strlen(messages_json), msgs_enc_len);
        if (!*msgs_enc)
        {
            log_error("save_session: encryption_encrypt failed for messages", NULL);
            abort_save = 1;
        }
    }

    if (!abort_save && metadata_json)
    {
        *meta_enc = encryption_encrypt(&sm->enc_key,
                                        (const unsigned char *)metadata_json,
                                        strlen(metadata_json), meta_enc_len);
        if (!*meta_enc)
        {
            log_error("save_session: encryption_encrypt failed for metadata", NULL);
            abort_save = 1;
        }
    }

    if (!abort_save && events_json)
    {
        *events_enc = encryption_encrypt(&sm->enc_key,
                                          (const unsigned char *)events_json,
                                          strlen(events_json), events_enc_len);
        if (!*events_enc)
        {
            log_error("save_session: encryption_encrypt failed for events", NULL);
            abort_save = 1;
        }
    }

    if (abort_save)
    {
        free(*msgs_enc); free(*meta_enc); free(*events_enc); free(*title_enc);
        *msgs_enc = NULL; *meta_enc = NULL; *events_enc = NULL; *title_enc = NULL;
        return -1;
    }
    return 0;
}

static int session_save_stmt(SessionManager *sm, Session *session, int mode,
                             unsigned char *title_enc, int title_enc_len,
                             unsigned char *msgs_enc, int msgs_enc_len,
                             unsigned char *meta_enc, int meta_enc_len,
                             unsigned char *events_enc, int events_enc_len)
{
    const char *sql_upsert =
        "INSERT INTO agent_sessions "
        "(id, title_encrypted, title_generation_attempted, created_at, "
        "messages_encrypted, metadata_encrypted, events_encrypted) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "title_encrypted=excluded.title_encrypted, "
        "title_generation_attempted=excluded.title_generation_attempted, "
        "created_at=excluded.created_at, "
        "messages_encrypted=excluded.messages_encrypted, "
        "metadata_encrypted=excluded.metadata_encrypted, "
        "events_encrypted=excluded.events_encrypted";
    const char *sql_insert_if_absent =
        "INSERT INTO agent_sessions "
        "(id, title_encrypted, title_generation_attempted, created_at, "
        "messages_encrypted, metadata_encrypted, events_encrypted) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO NOTHING";

    const char *sql = (mode == SM_INSERT_IF_ABSENT) ? sql_insert_if_absent
                                                    : sql_upsert;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("sqlite prepare save", "err", sqlite3_errmsg(sm->db), NULL);
        free(msgs_enc); free(meta_enc); free(events_enc); free(title_enc);
        return -1;
    }

    int bind_rc = sqlite3_bind_text(stmt, 1, session->id, -1, SQLITE_TRANSIENT);
    if (bind_rc == SQLITE_OK)
    {
        if (title_enc && title_enc_len > 0)
            bind_rc = sqlite3_bind_blob(stmt, 2, title_enc, title_enc_len, SQLITE_TRANSIENT);
        else
            bind_rc = sqlite3_bind_null(stmt, 2);
    }
    if (bind_rc == SQLITE_OK)
        bind_rc = sqlite3_bind_int(stmt, 3, session->title_generation_attempted);
    if (bind_rc == SQLITE_OK)
        bind_rc = sqlite3_bind_text(stmt, 4, session->created_at, -1, SQLITE_TRANSIENT);
    if (bind_rc == SQLITE_OK)
    {
        if (msgs_enc && msgs_enc_len > 0)
            bind_rc = sqlite3_bind_blob(stmt, 5, msgs_enc, msgs_enc_len, SQLITE_TRANSIENT);
        else
            bind_rc = sqlite3_bind_null(stmt, 5);
    }
    if (bind_rc == SQLITE_OK)
    {
        if (meta_enc && meta_enc_len > 0)
            bind_rc = sqlite3_bind_blob(stmt, 6, meta_enc, meta_enc_len, SQLITE_TRANSIENT);
        else
            bind_rc = sqlite3_bind_null(stmt, 6);
    }
    if (bind_rc == SQLITE_OK)
    {
        if (events_enc && events_enc_len > 0)
            bind_rc = sqlite3_bind_blob(stmt, 7, events_enc, events_enc_len, SQLITE_TRANSIENT);
        else
            bind_rc = sqlite3_bind_null(stmt, 7);
    }
    if (bind_rc != SQLITE_OK)
    {
        log_error("sqlite bind save", "err", sqlite3_errmsg(sm->db), NULL);
        sqlite3_finalize(stmt);
        free(msgs_enc); free(meta_enc); free(events_enc); free(title_enc);
        return -1;
    }

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
    {
        log_error("sqlite step save", "err", sqlite3_errmsg(sm->db), NULL);
        sqlite3_finalize(stmt);
        free(msgs_enc); free(meta_enc); free(events_enc); free(title_enc);
        return -1;
    }

    int changes = sqlite3_changes(sm->db);
    sqlite3_finalize(stmt);

    free(msgs_enc); free(meta_enc); free(events_enc); free(title_enc);

    if (mode == SM_INSERT_IF_ABSENT)
        return changes > 0 ? 1 : 0;
    return 0;
}

int save_session_core_locked(SessionManager *sm, Session *session,
                                 int mode)
{
    char *messages_json = session_serialize_messages_new(session);
    char *metadata_json = session_serialize_metadata_new(session);
    char *events_json = session_serialize_events_new(session);

    int abort_save = 0;
    if (!messages_json)
    {
        log_error("save_session: serialize messages failed (OOM)", NULL);
        abort_save = 1;
    }
    if (session->metadata && !metadata_json)
    {
        log_error("save_session: serialize metadata failed (OOM)", NULL);
        abort_save = 1;
    }
    if (session->events && !events_json)
    {
        log_error("save_session: serialize events failed (OOM)", NULL);
        abort_save = 1;
    }
    if (!abort_save && messages_json && strlen(messages_json) > (size_t)INT_MAX)
    {
        log_error("save_session: messages JSON exceeds INT_MAX bytes", NULL);
        abort_save = 1;
    }
    if (!abort_save && metadata_json && strlen(metadata_json) > (size_t)INT_MAX)
    {
        log_error("save_session: metadata JSON exceeds INT_MAX bytes", NULL);
        abort_save = 1;
    }
    if (!abort_save && events_json && strlen(events_json) > (size_t)INT_MAX)
    {
        log_error("save_session: events JSON exceeds INT_MAX bytes", NULL);
        abort_save = 1;
    }

    if (abort_save)
    {
        free(messages_json); free(metadata_json); free(events_json);
        return -1;
    }

    unsigned char *title_enc = NULL;
    int title_enc_len = 0;
    unsigned char *msgs_enc = NULL;
    int msgs_enc_len = 0;
    unsigned char *meta_enc = NULL;
    int meta_enc_len = 0;
    unsigned char *events_enc = NULL;
    int events_enc_len = 0;

    if (session_encrypt_all_fields(sm, session->title,
                                   messages_json, metadata_json, events_json,
                                   &title_enc, &title_enc_len,
                                   &msgs_enc, &msgs_enc_len,
                                   &meta_enc, &meta_enc_len,
                                   &events_enc, &events_enc_len) != 0)
    {
        free(messages_json); free(metadata_json); free(events_json);
        return -1;
    }

    int result = session_save_stmt(sm, session, mode,
                                   title_enc, title_enc_len,
                                   msgs_enc, msgs_enc_len,
                                   meta_enc, meta_enc_len,
                                   events_enc, events_enc_len);

    free(messages_json); free(metadata_json); free(events_json);
    return result;
}

char *session_manager_export_session_new(SessionManager *sm, const char *session_id)
{
    Session *s = session_manager_load_session_alloc(sm, session_id);
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

static Session *import_session_build(const char *json_str)
{

    cJSON *json = cJSON_Parse(json_str);
    if (!json) return NULL;

    /* C8: previously we did a `load_session` precheck here to refuse
     * duplicates, then called `session_manager_save_session` (INSERT OR
     * REPLACE) afterward. The two were separate mutex acquisitions, so a
     * concurrent thread could insert the same id in between — the precheck
     * would pass, the save would silently overwrite. The atomic
     * save_session_core(SM_INSERT_IF_ABSENT) does the existence-check + insert
     * as a single SQLite statement (`INSERT ... ON CONFLICT(id) DO NOTHING`)
     * while already holding sm->lock, so the "reject duplicates" promise is
     * now load-bearing. Duplicate-id detection is dropped from this function
     * because it would just duplicate the SQL-level check that follows. */

    Session *s = session_create("Imported Session");
    if (!s) {
        cJSON_Delete(json);
        return NULL;
    }

    cJSON *id_item = cJSON_GetObjectItem(json, "id");
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
        /* J1: `agent_save_session` filters out "system" messages before
         * persisting — the system prompt is regenerated by
         * `inject_system_with_summary` on every `agent_run_streaming_new`
         * cycle, so storing a stale copy would either (a) get
         * overwritten on the next run, leaking memory and confusing the
         * agent's messages array, or (b) if a future `agent_run_streaming_new`
         * skipped `inject_system_with_summary`, it would send a stale
         * system prompt. The import path used to bypass this filter
         * (it called `session_deserialize_messages` directly on the
         * imported JSON). Now we walk the parsed cJSON messages array
         * and DETACH any "system"-role entry before deserializing, so
         * what's written to the DB matches what agent_save_session
         * would have written itself. Iterate by index, decrement on
         * detach. The detached item is deleted, severing it from the
         * cJSON tree; the rest of the array shifts down to fill the gap. */
        for (int i = 0; i < cJSON_GetArraySize(msgs); )
        {
            cJSON *item = cJSON_GetArrayItem(msgs, i);
            cJSON *role = item ? cJSON_GetObjectItem(item, "role") : NULL;
            if (role && cJSON_IsString(role) &&
                strcmp(role->valuestring, "system") == 0)
            {
                cJSON_DetachItemFromArray(msgs, i);
                cJSON_Delete(item);
                continue; /* don't advance — array shifted down */
            }
            i++;
        }

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
    return s;
}

Session *session_manager_import_session_new(SessionManager *sm, const char *json_str)
{
    if (!sm || !sm->key_initialized || !json_str) return NULL;

    Session *s = import_session_build(json_str);
    if (!s) return NULL;

    /* C10: hold sm->lock across the insert so the import+check are atomic
     * at the mutex level too (the SQL-level DO NOTHING is already atomic
     * per C8, but the import builds a fresh Session outside the lock; with
     * the lock here, no other writer can race the build+insert pair). */
    session_manager_lock(sm);
    int rc = save_session_core_locked(sm, s, SM_INSERT_IF_ABSENT);
    if (rc <= 0)
    {
        session_manager_unlock(sm);
        session_free(s);
        return NULL;
    }
    session_manager_unlock(sm);

    return s;
}
