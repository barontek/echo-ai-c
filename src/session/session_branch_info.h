/*
 * session_branch_info.h - branch_info assembly contracts.
 * Depends on: session_manager.h.
 */

#ifndef ECHO_SESSION_BRANCH_INFO_H
#define ECHO_SESSION_BRANCH_INFO_H

#include "session_manager.h"
#include "session.h"

/* session_manager_branch_info_alloc is public API declared in
 * session_branch.h; this unit defines it. */

/**
 * branch_active_created_at - read the live chain's creation timestamp
 * @s: loaded session.
 *
 * Defined in session_branch.c; the info unit needs it for active-position
 * ordering.
 *
 * Return: caller-owned string (free()).
 */
char *branch_active_created_at(Session *s);

#endif /* ECHO_SESSION_BRANCH_INFO_H */
