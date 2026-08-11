/*
 * session_serialize.h - session row serialization contracts: the shared
 * load-with-decrypt and serialize+encrypt+save bodies. Public API
 * (save/load/import/export) stays declared in session_manager.h.
 * Depends on: session_manager.h.
 */

#ifndef ECHO_SESSION_SERIALIZE_H
#define ECHO_SESSION_SERIALIZE_H

#include "session_manager.h"

/* Save mode for save_session_core_locked. SM_UPSERT is the historical
 * "save-or-overwrite" behavior used by every caller that has already
 * loaded-or-created the row. SM_INSERT_IF_ABSENT is used by import_session,
 * where the "reject duplicates" promise must be enforced atomically via
 * INSERT ... ON CONFLICT(id) DO NOTHING. */
enum save_core_mode { SM_UPSERT, SM_INSERT_IF_ABSENT };

/**
 * load_session_locked - load and decrypt a session row
 * @sm: session manager; the caller MUST hold sm->lock.
 * @id: session id; must be non-empty.
 *
 * Return: caller-owned Session (free with session_free()), or NULL on
 * prepare/step/decrypt/parse failure. The caller must release the lock.
 */
Session *load_session_locked(SessionManager *sm, const char *id);

/**
 * save_session_core_locked - serialize, encrypt, and write a session row
 * @sm: session manager; the caller MUST hold sm->lock.
 * @session: session to persist (borrowed).
 * @mode: SM_UPSERT (save-or-overwrite) or SM_INSERT_IF_ABSENT (reject
 *   duplicates atomically).
 *
 * Return: 1 when a row was inserted in insert-if-absent mode, 0 on
 * update (or already-present), -1 on serialize/encrypt/SQL failure.
 */
int save_session_core_locked(SessionManager *sm, Session *session, int mode);

#endif /* ECHO_SESSION_SERIALIZE_H */
