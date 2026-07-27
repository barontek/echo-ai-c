#ifndef ECHO_SESSION_MANAGER_H
#define ECHO_SESSION_MANAGER_H

#include <sqlite3.h>
#include <pthread.h>
#include "session.h"
#include "encryption.h"

typedef struct {
    char *data_dir;
    sqlite3 *db;
    pthread_mutex_t lock;
    EncryptionKey enc_key;
    int key_initialized;
} SessionManager;

SessionManager *session_manager_create(const char *data_dir, const char *password);
void session_manager_free(SessionManager *sm);
Session *session_manager_create_session(SessionManager *sm, const char *title);
Session *session_manager_load_session(SessionManager *sm, const char *id);
int session_manager_save_session(SessionManager *sm, Session *session);
int session_manager_delete_session(SessionManager *sm, const char *id);

typedef struct {
    char **ids;
    char **titles;
    char **created_ats;
    int *title_generation_attempteds;
    int count;
} SessionList;

SessionList *session_manager_list_sessions(SessionManager *sm);
void session_list_free(SessionList *list);

/* new session operations */
int session_manager_add_message(SessionManager *sm, const char *session_id,
                                 const char *role, const char *content,
                                 const char *tool_call_id, const char *tool_name);
int session_manager_truncate_history(SessionManager *sm, const char *session_id, int index);
char *session_manager_export_session(SessionManager *sm, const char *session_id);
Session *session_manager_import_session(SessionManager *sm, const char *json_str);
int session_manager_purge_sessions(SessionManager *sm, int older_than_days);
int session_manager_log_event(SessionManager *sm, const char *session_id,
                               const char *event_type, const char *data);

int migration_change_password(SessionManager *sm, const char *new_password);

/* C10: Public mutex acquire/release so callers in other TUs (agent.c,
 * migration.c) can hold sm->lock across a load-modify-save triad.
 * sm->lock is PTHREAD_MUTEX_NORMAL (default-initialized) — re-entrant
 * misuse deadlocks immediately, by design. */
void session_manager_lock(SessionManager *sm);
void session_manager_unlock(SessionManager *sm);

/* C10: _nolock variants for when the caller already holds sm->lock.
 * Caller MUST hold sm->lock for the duration of this call; do not
 * call any non-_nolock session_manager_* API from within — the
 * non-recursive PTHREAD_MUTEX_NORMAL will deadlock on re-entry,
 * by design. */
Session *session_manager_load_session_nolock(SessionManager *sm, const char *id);
int session_manager_save_session_nolock(SessionManager *sm, Session *session);

#ifdef SESSION_MANAGER_TEST
void session_manager_test_set_alloc_fail(int nth_allocation);
void session_manager_test_set_bind_fail(int nth_bind);
void session_manager_test_set_encrypt_fail(int nth_encrypt);
#endif

#endif
