#ifndef ECHO_SESSION_MANAGER_H
#define ECHO_SESSION_MANAGER_H

#include "session.h"
#include "encryption.h"

typedef struct {
    char *data_dir;
    EncryptionKey enc_key;
    int key_initialized;
} SessionManager;

SessionManager *session_manager_create(const char *data_dir, const char *password);
void session_manager_free(SessionManager *sm);
int session_manager_ensure_dirs(SessionManager *sm);
Session *session_manager_create_session(SessionManager *sm, const char *title);
Session *session_manager_load_session(SessionManager *sm, const char *id);
int session_manager_save_session(SessionManager *sm, Session *session);
int session_manager_delete_session(SessionManager *sm, const char *id);

typedef struct {
    char **ids;
    char **titles;
    char **created_ats;
    int count;
} SessionList;

SessionList *session_manager_list_sessions(SessionManager *sm);
void session_list_free(SessionList *list);
int migration_change_password(SessionManager *sm, const char *new_password);

#endif
