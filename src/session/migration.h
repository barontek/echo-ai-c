/*
 * migration.h - crash-safe password migration for the session store:
 * re-encrypts all owned rows under a new password and recovers an
 * interrupted migration on the next startup.
 * Depends on: session_manager.h, encryption.
 */

#ifndef ECHO_MIGRATION_H
#define ECHO_MIGRATION_H

#include "session_manager.h"

/**
 * migration_check_and_recover - finish or undo an interrupted password
 * migration
 * @sm: manager whose data_dir is inspected; must be non-NULL with a
 *   non-NULL data_dir. Borrowed for the call duration.
 * @password: current password, NUL-terminated; must be non-NULL, borrowed
 *   for the call duration. Used to re-derive keys when deciding which salt
 *   the DB was actually committed with.
 *
 * Called before any key use (session_manager_create_ex -> init_encryption).
 * With no marker file (.changing_pwd absent) this is a no-op. With a
 * marker, the migration is finalized in whichever direction it had
 * progressed so the on-disk salt/verifier pair always ends up consistent
 * with the DB's committed salt.
 *
 * Return: 0 on success or no-op, -1 on I/O, SQL, or allocation failure
 * (log lines carry context; not all paths log). Thread-safe only against
 * other managers — not against concurrent use of the same one.
 */
int migration_check_and_recover(SessionManager *sm, const char *password);

/**
 * migration_change_password - re-encrypt all owned rows under a new password
 * @sm: manager with a key-initialized, non-NULL db and data_dir.
 * @new_password: replacement password, NUL-terminated; must be non-NULL,
 *   borrowed for the call duration.
 *
 * Takes sm->lock for the whole operation. The migration is crash-safe and
 * all-or-nothing: either every owned row is re-encrypted under the new
 * password and the on-disk salt/verifier files match it, or the old state
 * is fully restored (the DB is never left half-migrated).
 *
 * Return: 0 on success; -1 on failure (old state restored, enc_key stays
 * the old key); -2 when the DB commit succeeded but activating the new
 * verifier failed — the swap is retried once in-process through the same
 * recovery path a restart would run, and only if that retry also fails is
 * enc_key left as the NEW key, the on-disk verifier still matching the
 * old password, and the marker file kept so the next startup's
 * migration_check_and_recover() can complete the swap. Errors are logged.
 * Thread-safe with respect to other session_manager_* calls on the same
 * manager (all take the same mutex).
 */
int migration_change_password(SessionManager *sm, const char *new_password);

#ifdef SESSION_MANAGER_TEST
/**
 * migration_test_set_rename_fail - make the Nth rename() call fail here
 * @nth_rename: 1-based index of the rename() call to fail; -1 disables
 *   fault injection.
 *
 * Test-only hook for the verifier-activation failure path
 * (migration_change_password returning -2). Resets the call counter on
 * every arm; the index counts from the next migration call onward.
 *
 * Return: void; never fails.
 */
void migration_test_set_rename_fail(int nth_rename);
#endif

#endif
