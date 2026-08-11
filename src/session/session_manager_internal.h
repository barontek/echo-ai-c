/*
 * session_manager_internal.h - shared contracts for the session store
 * split across session_manager/db/serialize/list/purge units. Documented
 * exception to the one-header-per-module rule: the internal lock helpers
 * and the SESSION_MANAGER_TEST fault-injection shims cross unit
 * boundaries, so their declarations live here instead of being duplicated
 * per file. The str_dup/realloc/asprintf/sqlite3 bind and encryption
 * encrypt #defines that route calls through the shims stay in each TU (after its
 * includes), matching the session_branch.c/migration.c pattern, so shim
 * bodies never see the macros. Not installed or included outside
 * src/session.
 * Depends on: session_manager.h, sqlite3, encryption.h.
 */

#ifndef ECHO_SESSION_MANAGER_INTERNAL_H
#define ECHO_SESSION_MANAGER_INTERNAL_H

#include <sqlite3.h>

#include "session_manager.h"
#include "encryption.h"

#ifdef SESSION_MANAGER_TEST
/* Fault-injection shims defined non-static in session_manager.c (under
 * the same guard): session_manager.c owns the counters, and every session
 * TU compiled with SESSION_MANAGER_TEST routes its str_dup, realloc,
 * asprintf, sqlite3_bind_* and encryption_encrypt calls through them so
 * a single knob reaches the whole module. Production builds never see
 * these symbols. */
char *sm_test_strdup(const char *s);
void *sm_test_realloc(void *ptr, size_t size);
int sm_test_asprintf(char **strp, const char *fmt, ...);
int sm_test_bind_text(sqlite3_stmt *s, int idx, const char *t, int n,
                      void (*del)(void *));
int sm_test_bind_int(sqlite3_stmt *s, int idx, int v);
int sm_test_bind_blob(sqlite3_stmt *s, int idx, const void *p, int n,
                      void (*del)(void *));
int sm_test_bind_null(sqlite3_stmt *s, int idx);
unsigned char *sm_test_encrypt(const EncryptionKey *key,
                               const unsigned char *pt, int pt_len,
                               int *out_len);
#endif

/* Internal lock/unlock (raw pthread calls that discard the return
 * value). A failed lock/unlock cannot be recovered from — the response
 * is a loud log, not silence. Defined in session_manager.c. */
void sm_lock(SessionManager *sm);
void sm_unlock(SessionManager *sm);

#endif /* ECHO_SESSION_MANAGER_INTERNAL_H */
