#ifndef ECHO_MIGRATION_H
#define ECHO_MIGRATION_H

#include "session_manager.h"

int migration_check_and_recover(SessionManager *sm);
int migration_change_password(SessionManager *sm, const char *new_password);

#endif
