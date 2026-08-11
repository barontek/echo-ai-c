/*
 * session_list.c - session listing: row iteration with
 * parallel-array growth and title decryption.
 * Depends on: sqlite3, encryption, session_manager types.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "session_list.h"
#include "session_manager_internal.h"
#include "session_manager.h"
#include "encryption.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

#ifdef SESSION_MANAGER_TEST
/* Shared fault-injection hooks (counters live in session_manager.c). */
#define str_dup sm_test_strdup
#define realloc sm_test_realloc
#endif


static int session_list_grow(SessionList *list, int *capacity)
{
    int new_cap = *capacity * 2;
    char **new_ids = realloc(list->ids, sizeof(char *) * (size_t)new_cap);
    if (!new_ids) return -1;
    list->ids = new_ids;

    char **new_titles = realloc(list->titles, sizeof(char *) * (size_t)new_cap);
    if (!new_titles) return -1;
    list->titles = new_titles;

    char **new_dates = realloc(list->created_ats, sizeof(char *) * (size_t)new_cap);
    if (!new_dates) return -1;
    list->created_ats = new_dates;

    int *new_tgas = realloc(list->title_generation_attempteds,
                            sizeof(int) * (size_t)new_cap);
    if (!new_tgas) return -1;
    list->title_generation_attempteds = new_tgas;

    *capacity = new_cap;
    return 0;
}

static int session_list_append_row(SessionList *list, sqlite3_stmt *stmt,
                                   const EncryptionKey *key)
{
    const char *id = (const char *)sqlite3_column_text(stmt, 0);
    const void *title_blob = sqlite3_column_blob(stmt, 1);
    int title_len = sqlite3_column_bytes(stmt, 1);
    int tga = sqlite3_column_int(stmt, 2);
    const char *created = (const char *)sqlite3_column_text(stmt, 3);

    char *title_dup = NULL;
    unsigned char *dec = NULL;
    int dec_len = 0;
    if (title_blob && title_len > 0)
        dec = encryption_decrypt(key, title_blob, title_len, &dec_len);
    if (dec)
    {
        title_dup = str_dup((const char *)dec);
        free(dec);
    }
    else
    {
        title_dup = str_dup("");
    }

    char *id_dup = str_dup(id ? id : "");
    char *created_dup = str_dup(created ? created : "");
    if (!id_dup || !title_dup || !created_dup)
    {
        /* B6: str_dup failure must not silently truncate; free locals and
         * return -1 so the caller knows it failed. */
        free(id_dup);
        free(title_dup);
        free(created_dup);
        return -1;
    }
    list->ids[list->count] = id_dup;
    list->titles[list->count] = title_dup;
    list->created_ats[list->count] = created_dup;
    list->title_generation_attempteds[list->count] = tga;
    list->count++;
    return 0;
}

SessionList *session_manager_list_sessions(SessionManager *sm)
{
    if (!sm || !sm->data_dir || !sm->db || !sm->key_initialized) return NULL;

    sm_lock(sm);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, title_encrypted, title_generation_attempted, created_at "
                      "FROM agent_sessions ORDER BY created_at DESC";
    if (sqlite3_prepare_v2(sm->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("sqlite prepare list", "err", sqlite3_errmsg(sm->db), NULL);
        sm_unlock(sm);
        return NULL;
    }

    SessionList *list = calloc(1, sizeof(SessionList));
    if (!list) {
        sqlite3_finalize(stmt);
        sm_unlock(sm);
        return NULL;
    }

    int capacity = 16;
    list->ids = malloc(sizeof(char *) * (size_t)capacity);
    list->titles = malloc(sizeof(char *) * (size_t)capacity);
    list->created_ats = malloc(sizeof(char *) * (size_t)capacity);
    list->title_generation_attempteds = malloc(sizeof(int) * (size_t)capacity);
    if (!list->ids || !list->titles || !list->created_ats || !list->title_generation_attempteds)
    {
        sqlite3_finalize(stmt);
        sm_unlock(sm);
        session_list_free(list);
        return NULL;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        if (list->count >= capacity && session_list_grow(list, &capacity) != 0)
        {
            sqlite3_finalize(stmt);
            sm_unlock(sm);
            session_list_free(list);
            return NULL;
        }
        if (session_list_append_row(list, stmt, &sm->enc_key) != 0)
        {
            sqlite3_finalize(stmt);
            sm_unlock(sm);
            session_list_free(list);
            return NULL;
        }
    }

    sqlite3_finalize(stmt);
    sm_unlock(sm);
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
