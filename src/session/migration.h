#ifndef ECHO_MIGRATION_H
#define ECHO_MIGRATION_H

#include "session_manager.h"

/* Recovers interrupted password migration files; returns -1 on I/O/SQL error. */
int migration_check_and_recover(SessionManager *sm, const char *password);

/* Re-encrypts all owned rows atomically; new_password remains caller-owned. */
int migration_change_password(SessionManager *sm, const char *new_password);

#endif
