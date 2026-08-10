/*
 * session_manager.h - refcounted, mutex-protected session store: sqlite
 * persistence of encrypted session blobs, oauth credential storage,
 * message append/truncate, branch fork/switch, import/export, and purge.
 * Depends on: sqlite3, pthreads, session.h, encryption.h.
 */

#ifndef ECHO_SESSION_MANAGER_H
#define ECHO_SESSION_MANAGER_H

#include <sqlite3.h>
#include <pthread.h>
#include <stdatomic.h>
#include "session.h"
#include "encryption.h"

/* A manager owns data_dir and db and guards them with lock. enc_key holds
 * the current key material (scrubbed when the manager is destroyed);
 * key_initialized is 1 once a password has been derived. ref_count counts
 * live references; the last session_manager_free() closes the db, destroys
 * the mutex, zeroes the struct, and frees it. */
typedef struct {
    char *data_dir;
    sqlite3 *db;
    pthread_mutex_t lock;
    EncryptionKey enc_key;
    int key_initialized;
    atomic_uint ref_count;
} SessionManager;

typedef enum {
    PROVIDER_OAUTH_LOAD_OK = 0,
    PROVIDER_OAUTH_LOAD_NOT_FOUND,
    PROVIDER_OAUTH_LOAD_INVALID_ARGUMENT,
    PROVIDER_OAUTH_LOAD_SQL_ERROR,
    PROVIDER_OAUTH_LOAD_DECRYPT_ERROR,
    PROVIDER_OAUTH_LOAD_OOM
} ProviderOAuthLoadResult;

typedef enum {
    SESSION_MANAGER_CREATE_OK = 0,
    SESSION_MANAGER_CREATE_AUTH_FAILED,
    SESSION_MANAGER_CREATE_STORAGE_FAILED
} SessionManagerCreateResult;

/**
 * session_manager_create - open (or initialize) the session store
 * @data_dir: directory holding the salt, verifier, and echo-ai.db;
 *   borrowed for the call duration; must be non-NULL. Created if absent.
 * @password: password for key derivation and verifier check; borrowed for
 *   the call duration; must be non-NULL.
 *
 * Convenience wrapper over session_manager_create_ex() with a NULL result
 * pointer — on authentication failure the two are indistinguishable (NULL).
 *
 * Return: caller-owned SessionManager (free with session_manager_free()),
 * or NULL on any failure. See session_manager_create_ex().
 */
SessionManager *session_manager_create(const char *data_dir, const char *password);

/**
 * session_manager_create_ex - open (or initialize) the session store with
 * failure classification
 * @data_dir: directory holding the salt, verifier, and echo-ai.db;
 *   borrowed for the call duration; must be non-NULL. Created if absent.
 * @password: password for key derivation and verifier check; borrowed for
 *   the call duration; must be non-NULL.
 * @result: out-param receiving the failure class, or NULL to ignore.
 *   Always written: SESSION_MANAGER_CREATE_OK on success,
 *   SESSION_MANAGER_CREATE_AUTH_FAILED when the verifier does not match
 *   the password, SESSION_MANAGER_CREATE_STORAGE_FAILED otherwise.
 *
 * Runs interrupted-password-migration recovery, ensures a salt file,
 * derives the key, verifies (or on first run creates) the password
 * verifier, then opens echo-ai.db and creates the agent_sessions,
 * provider_oauth, and user_memory tables. Runs the migration-recovery and
 * salt/verifier file steps WITHOUT holding the mutex — it is meant to run
 * once at startup, before the manager is shared.
 *
 * Return: caller-owned SessionManager (free with session_manager_free()),
 * or NULL on any failure (in which case *result tells the failure class).
 * No shared state; concurrent creates against the same data_dir are
 * serialized only by the filesystem (salt is created with "wbx").
 */
SessionManager *session_manager_create_ex(const char *data_dir,
                                          const char *password,
                                          SessionManagerCreateResult *result);

/**
 * session_manager_retain - take an additional reference on a manager
 * @sm: manager to retain; must be non-NULL.
 *
 * Thread-safe (atomic refcount). Each successful retain must be balanced
 * by one session_manager_free().
 *
 * Return: @sm on success, or NULL when @sm has already been released
 * (refcount 0) or its refcount is exhausted (UINT_MAX).
 */
SessionManager *session_manager_retain(SessionManager *sm);

/**
 * session_manager_free - drop one reference on a manager
 * @sm: manager to release, or NULL (no-op).
 *
 * Decrements the refcount; only the last reference destroys: closes the
 * sqlite connection, destroys the mutex, zeroes the struct (scrubbing
 * enc_key), and frees it. Thread-safe (atomic refcount). After the last
 * release @sm is dangling.
 *
 * Return: void.
 */
void session_manager_free(SessionManager *sm);

/**
 * session_manager_create_session - create and persist a session
 * @sm: manager; must be non-NULL with key_initialized.
 * @title: title for session_create(); borrowed, NULL yields the default.
 *
 * Creates the session in memory and immediately persists it (encrypted)
 * under sm->lock.
 *
 * Return: caller-owned Session (free with session_free()), or NULL on
 * invalid manager, session_create() failure, or save failure (the created
 * session is freed internally on save failure). Thread-safe via sm->lock.
 */
Session *session_manager_create_session(SessionManager *sm, const char *title);

/**
 * session_manager_load_session - load and decrypt a session
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @id: session id; must be non-NULL and non-empty; borrowed.
 *
 * Holds sm->lock while loading. Any field that fails to decrypt or parse
 * fails the whole load (NULL). Returns NULL for both "not found" and
 * error — check the logs for the distinction.
 *
 * Return: caller-owned Session (free with session_free()), or NULL when
 * the id is absent, on invalid arguments, or a decrypt/parse/allocation
 * failure. Thread-safe via sm->lock.
 */
Session *session_manager_load_session(SessionManager *sm, const char *id);

/**
 * session_manager_save_session - encrypt and upsert a session
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @session: session to persist; must be non-NULL with a non-NULL non-empty
 *   id. Borrowed — ownership stays with the caller.
 *
 * Holds sm->lock. Serializes messages/metadata/events, encrypts each blob
 * with sm->enc_key, and upserts the row. An empty title is stored as SQL
 * NULL. Refuses the save (with logs) on serialization or encryption OOM.
 *
 * Return: 0 on success, -1 on invalid arguments, serialization/encryption
 * failure, or SQLite error. Thread-safe via sm->lock.
 */
int session_manager_save_session(SessionManager *sm, Session *session);

/**
 * session_manager_delete_session - delete a session row
 * @sm: manager; must be non-NULL with db. key_initialized is NOT required
 *   — delete never decrypts (deliberate asymmetry vs load/save).
 * @id: session id; must be non-NULL; borrowed.
 *
 * Holds sm->lock.
 *
 * Return: 1 if a row was deleted, 0 if no row matched (caller should treat
 * as 404), -1 on SQLite error. Thread-safe via sm->lock.
 */
int session_manager_delete_session(SessionManager *sm, const char *id);

/* Parallel arrays of session list rows; every string and the arrays
 * themselves are owned by the list. Freed by session_list_free(). */
typedef struct {
    char **ids;
    char **titles;
    char **created_ats;
    int *title_generation_attempteds;
    int count;
} SessionList;

/**
 * session_manager_list_sessions - list all sessions, newest first
 * @sm: manager; must be non-NULL with key_initialized and db.
 *
 * Holds sm->lock. Orders by created_at DESC and decrypts each title (a row
 * whose title fails to decrypt is listed with an empty title).
 *
 * Return: caller-owned SessionList (free with session_list_free()), or
 * NULL on invalid arguments, SQLite error, or any allocation failure.
 * Thread-safe via sm->lock.
 */
SessionList *session_manager_list_sessions(SessionManager *sm);

/**
 * session_list_free - free a list from session_manager_list_sessions()
 * @list: list to free, or NULL (no-op).
 *
 * Frees every string in each parallel array, the arrays, and the struct.
 * After the call @list is dangling.
 *
 * Return: void.
 */
void session_list_free(SessionList *list);

/**
 * session_manager_add_message - append a message to a session
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @session_id: session id; must be non-NULL; borrowed.
 * @role: message role; NULL becomes "".
 * @content: message content; NULL becomes "".
 * @tool_call_id: optional tool-call id; NULL when not a tool call.
 * @tool_name: optional tool name; NULL when not a tool call.
 *
 * Holds sm->lock across the load, append, save triad so no other thread
 * can interleave a save on the same row. Fails the whole operation if any
 * allocation fails (the message count is rolled back).
 *
 * Return: 0 on success, -1 on missing session, allocation failure, or save
 * failure. Thread-safe via sm->lock.
 */
int session_manager_add_message(SessionManager *sm, const char *session_id,
                                 const char *role, const char *content,
                                 const char *tool_call_id, const char *tool_name);

/**
 * session_manager_truncate_history - drop messages from an index on
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @session_id: session id; must be non-NULL; borrowed.
 * @index: first message to drop; must be in [0, messages_count).
 *
 * Keeps messages [0, index) and clears the rest, then saves. Holds sm->lock
 * across the load, truncate, save triad.
 *
 * Return: 0 on success, -1 on missing session, out-of-range index, or save
 * failure. Thread-safe via sm->lock.
 */
int session_manager_truncate_history(SessionManager *sm, const char *session_id, int index);

/* Result of a fork: the old chain's snapshot record id plus the minted
 * identity of the new fork message. All strings are caller-owned (free
 * individually); fork_message is a deep copy the caller owns
 * (message_clear() it after use). */
typedef struct {
    char *branch_id;
    char *fork_message_id;
    char *fork_group_id;
    Message fork_message;
} SessionManagerForkResult;

/**
 * session_manager_fork_branch - fork the live chain at a message
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @session_id: session id; must be non-NULL; borrowed.
 * @message_id: fork point by message id (exact match scan), or NULL to
 *   fall back to @index.
 * @index: fork point by position, used when @message_id is NULL or does
 *   not match; must be in bounds.
 * @new_content: replacement content for the forked message, or NULL to
 *   keep the original's content.
 * @out: result struct; must be non-NULL. Zeroed on entry and on failure.
 *
 * Forks the live chain of @session_id at the resolved message. The
 * pre-fork chain is deep-copied into a snapshot record under
 * metadata.branches.list (never mutated afterwards); the live array is
 * truncated after the fork point; the fork message is replaced by a deep
 * copy with freshly minted id + fork_group_id and (when non-NULL) new
 * content; the pre-fork message at the fork point shares the same minted
 * fork_group_id (and gets an id minted if it lacked one) so the pill
 * renders on both chains. Re-forking a point that already carries a group
 * JOINS that group instead of minting a fresh one. All-or-nothing: any
 * allocation failure leaves the session unchanged on disk. Holds sm->lock.
 *
 * Return: 0 on success and *out filled; -1 on any failure with *out
 * zeroed. Thread-safe via sm->lock.
 */
int session_manager_fork_branch(SessionManager *sm, const char *session_id,
                                const char *message_id, int index,
                                const char *new_content,
                                SessionManagerForkResult *out);

/**
 * session_manager_switch_branch - make a snapshot chain live
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @session_id: session id; must be non-NULL; borrowed.
 * @branch_id: id of the snapshot record to activate; must be non-NULL;
 *   borrowed.
 *
 * Switches the live chain of @session_id to the snapshot stored under
 * @branch_id. The current live chain is first snapshotted (a fresh record
 * is always appended — records are never replaced by anchor match, since
 * sibling chains share the fork point's id), so no chain is ever lost.
 * Switching to the chain that is already live is a no-op. Holds sm->lock.
 *
 * Return: 0 on success (including the no-op), -1 on unknown branch, load,
 * or allocation failure (session unchanged on disk). Thread-safe via
 * sm->lock.
 */
int session_manager_switch_branch(SessionManager *sm, const char *session_id,
                                  const char *branch_id);

/**
 * session_manager_tag_message - mark a message as a chain fork point
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @session_id: session id; must be non-NULL; borrowed.
 * @index: position of the message to tag; must be in bounds.
 * @fork_group_id: group id to stamp onto the message; must be non-NULL;
 *   borrowed.
 *
 * Tags the message at @index with @fork_group_id plus a freshly minted id,
 * marking it as the chain's fork point (used for the regenerated assistant
 * response after a regenerate fork). A message that already carries a
 * fork_group_id keeps it. All-or-nothing: on any failure the session is
 * unchanged on disk and NULL is returned. Holds sm->lock.
 *
 * Return: caller-owned minted message id (free with free()), or NULL on
 * any failure. Thread-safe via sm->lock.
 */
char *session_manager_tag_message(SessionManager *sm, const char *session_id,
                                  int index, const char *fork_group_id);

/**
 * session_manager_branch_info_alloc - build the branch_info JSON array
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @session_id: session id; must be non-NULL; borrowed.
 *
 * Builds [{message_id, count, active, branch_ids}] with one entry per fork
 * point on the live chain (a message carrying a fork_group_id). count =
 * number of chains (snapshot records + live) containing a message with
 * that fork_group_id; active = 1-based position of the live chain among
 * them, ordered by chain creation time; branch_ids lists the non-live
 * chains' record ids in that order (the frontend's switch target).
 * Sessions without metadata.branches yield an empty array. Holds sm->lock.
 *
 * Return: caller-owned JSON string (free with free()), or NULL on OOM or
 * load failure. Thread-safe via sm->lock.
 */
char *session_manager_branch_info_alloc(SessionManager *sm, const char *session_id);

/**
 * session_manager_export_session - export a session as plaintext JSON
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @session_id: session id; must be non-NULL; borrowed.
 *
 * Loads the session and serializes {id, title, title_generation_attempted,
 * created_at, messages, metadata, events}; optional fields are omitted
 * when empty. The result is decrypted plaintext — the caller is
 * responsible for treating it as sensitive.
 *
 * Return: caller-owned JSON string (free with free()), or NULL on load
 * failure or OOM. Thread-safe via sm->lock (held during the load).
 */
char *session_manager_export_session(SessionManager *sm, const char *session_id);

/**
 * session_manager_import_session - import a session from plaintext JSON
 * @sm: manager; must be non-NULL with key_initialized.
 * @json_str: exported session JSON; must be non-NULL; borrowed.
 *
 * Parses the JSON into a fresh Session, dropping any "system"-role
 * messages (the system prompt is regenerated per run, mirroring what
 * agent_save_session persists), then inserts the row with an atomic
 * INSERT ... ON CONFLICT(id) DO NOTHING while holding sm->lock — a
 * duplicate id is rejected, never overwritten.
 *
 * Return: caller-owned Session (free with session_free()), or NULL on
 * invalid arguments, parse failure, duplicate id, OOM, or save failure.
 * Thread-safe via sm->lock.
 */
Session *session_manager_import_session(SessionManager *sm, const char *json_str);

/**
 * session_manager_purge_sessions - delete sessions older than N days
 * @sm: manager; must be non-NULL with db.
 * @older_than_days: age threshold in days; must be in [0, 36500] — values
 *   outside the range are refused to avoid overflow in the day-to-second
 *   conversion.
 *
 * Deletes agent_sessions rows whose created_at sorts before the cutoff.
 * Holds sm->lock.
 *
 * Return: number of rows deleted (possibly 0), or -1 on invalid arguments,
 * an out-of-range @older_than_days, or SQLite error. Thread-safe via
 * sm->lock.
 */
int session_manager_purge_sessions(SessionManager *sm, int older_than_days);

/**
 * session_manager_log_event - append an event to a session's events array
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @session_id: session id; must be non-NULL; borrowed.
 * @event_type: event type string; NULL becomes "".
 * @data: event payload string; NULL becomes "".
 *
 * Appends {event_type, data, timestamp} to session->events and saves.
 * Holds sm->lock across the load, append, save triad.
 *
 * Return: 0 on success, -1 on missing session or save failure. Thread-safe
 * via sm->lock.
 */
int session_manager_log_event(SessionManager *sm, const char *session_id,
                               const char *event_type, const char *data);

/**
 * session_manager_save_provider_oauth - store encrypted provider
 * credentials
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @provider_name: provider key (e.g. "github"); must be non-NULL and
 *   non-empty; borrowed.
 * @data: credential payload; must be non-NULL and non-empty; borrowed.
 *
 * Encrypts @data with sm->enc_key and upserts the provider_oauth row.
 * Holds sm->lock. The caller retains ownership of @data.
 *
 * Return: 0 on success, -1 on invalid arguments, encryption failure, SQL
 * error, or OOM (logged). Thread-safe via sm->lock.
 */
int session_manager_save_provider_oauth(SessionManager *sm,
                                        const char *provider_name,
                                        const char *data);

/**
 * session_manager_load_provider_oauth_ex - load and decrypt provider
 * credentials with failure classification
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @provider_name: provider key; must be non-NULL and non-empty; borrowed.
 * @data_out: out-param receiving the caller-owned NUL-terminated payload;
 *   must be non-NULL. Set to NULL on any failure.
 *
 * Holds sm->lock. Distinguishes NOT_FOUND from SQL and decrypt failures.
 * The decrypted payload must contain no embedded NUL.
 *
 * Return: PROVIDER_OAUTH_LOAD_OK with *data_out caller-owned (free with
 * free()); otherwise the failure class with *data_out NULL. Thread-safe
 * via sm->lock.
 */
ProviderOAuthLoadResult session_manager_load_provider_oauth_ex(
    SessionManager *sm, const char *provider_name, char **data_out);

/**
 * session_manager_load_provider_oauth - load provider credentials
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @provider_name: provider key; must be non-NULL and non-empty; borrowed.
 *
 * Convenience wrapper over session_manager_load_provider_oauth_ex(): any
 * failure (including not-found) collapses to NULL.
 *
 * Return: caller-owned NUL-terminated payload (free with free()), or NULL
 * on any failure. Thread-safe via sm->lock.
 */
char *session_manager_load_provider_oauth(SessionManager *sm,
                                           const char *provider_name);

/**
 * session_manager_delete_provider_oauth - delete stored provider
 * credentials
 * @sm: manager; must be non-NULL with db. key_initialized is not required
 *   (delete never decrypts).
 * @provider_name: provider key; must be non-NULL and non-empty; borrowed.
 *
 * Holds sm->lock.
 *
 * Return: 0 on success (whether or not a row matched), -1 on invalid
 * arguments or SQLite error. Thread-safe via sm->lock.
 */
int session_manager_delete_provider_oauth(SessionManager *sm,
                                          const char *provider_name);

/**
 * migration_change_password - re-encrypt all owned rows under a new
 * password
 * @sm: manager with a key-initialized, non-NULL db and data_dir.
 * @new_password: replacement password; must be non-NULL, borrowed for the
 *   call duration.
 *
 * Full contract in migration.h: 0 on success, -1 on failure (old state
 * restored), -2 when the DB was committed but verifier activation failed
 * (enc_key stays the new key; the next startup recovers via the marker).
 */
int migration_change_password(SessionManager *sm, const char *new_password);

/**
 * session_manager_lock - acquire the manager mutex
 * @sm: manager; NULL is a no-op.
 *
 * Public lock so callers in other translation units (agent.c, migration.c)
 * can hold sm->lock across a load-modify-save triad. The mutex is
 * PTHREAD_MUTEX_NORMAL (non-recursive): re-entrant misuse deadlocks
 * immediately, by design. Do not call any non-_nolock session_manager_*
 * function while holding it.
 *
 * Return: void.
 */
void session_manager_lock(SessionManager *sm);

/**
 * session_manager_unlock - release the manager mutex
 * @sm: manager; NULL is a no-op.
 *
 * Counterpart of session_manager_lock(). Must be called on every locked
 * path.
 *
 * Return: void.
 */
void session_manager_unlock(SessionManager *sm);

/**
 * session_manager_load_session_nolock - load a session while already
 * holding sm->lock
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @id: session id; must be non-NULL and non-empty; borrowed.
 *
 * Same contract as session_manager_load_session() but does not take the
 * mutex. The caller MUST hold sm->lock for the duration of the call — do
 * not call any non-_nolock session_manager_* API from within, or the
 * non-recursive mutex deadlocks.
 *
 * Return: caller-owned Session (free with session_free()), or NULL on
 * missing session, invalid arguments, or decrypt/parse/allocation failure.
 */
Session *session_manager_load_session_nolock(SessionManager *sm, const char *id);

/**
 * session_manager_save_session_nolock - save a session while already
 * holding sm->lock
 * @sm: manager; must be non-NULL with key_initialized and db.
 * @session: session to persist; must be non-NULL with a non-NULL non-empty
 *   id. Borrowed — ownership stays with the caller.
 *
 * Same contract as session_manager_save_session() but does not take the
 * mutex. The caller MUST hold sm->lock for the duration of the call — do
 * not call any non-_nolock session_manager_* API from within, or the
 * non-recursive mutex deadlocks.
 *
 * Return: 0 on success, -1 on invalid arguments, serialization/encryption
 * failure, or SQLite error.
 */
int session_manager_save_session_nolock(SessionManager *sm, Session *session);

#ifdef SESSION_MANAGER_TEST
/**
 * session_manager_test_set_alloc_fail - arm str_dup fault injection
 * @nth_allocation: fail the Nth str_dup call (1-based), or -1 to disarm.
 *
 * Test-only hook: makes the Nth str_dup inside this module return NULL so
 * allocation-failure paths can be exercised deterministically. Only
 * compiled with -DSESSION_MANAGER_TEST=1.
 *
 * Return: void. Not thread-safe; call before any concurrent use.
 */
void session_manager_test_set_alloc_fail(int nth_allocation);

/**
 * session_manager_test_set_realloc_fail - arm realloc fault injection
 * @nth_allocation: fail the Nth realloc call (1-based), or -1 to disarm.
 *
 * Test-only hook: makes the Nth realloc inside this module return NULL.
 * Only compiled with -DSESSION_MANAGER_TEST=1.
 *
 * Return: void. Not thread-safe; call before any concurrent use.
 */
void session_manager_test_set_realloc_fail(int nth_allocation);

/**
 * session_manager_test_set_bind_fail - arm sqlite3_bind_* fault injection
 * @nth_bind: make the Nth bind call (across bind_text/bind_int/
 *   bind_blob/bind_null) return SQLITE_NOMEM, or -1 to disarm.
 *
 * Test-only hook proving the save path checks bind results. Only compiled
 * with -DSESSION_MANAGER_TEST=1.
 *
 * Return: void. Not thread-safe; call before any concurrent use.
 */
void session_manager_test_set_bind_fail(int nth_bind);

/**
 * session_manager_test_set_encrypt_fail - arm encryption_encrypt fault
 * injection
 * @nth_encrypt: make the Nth encryption_encrypt call return NULL, or -1
 *   to disarm.
 *
 * Test-only hook proving the save path checks encrypt failures instead of
 * letting a NULL blob bind silently. Only compiled with
 * -DSESSION_MANAGER_TEST=1.
 *
 * Return: void. Not thread-safe; call before any concurrent use.
 */
void session_manager_test_set_encrypt_fail(int nth_encrypt);

/**
 * session_manager_test_set_oauth_alloc_fail - arm oauth result malloc
 * fault injection
 * @nth_allocation: fail the Nth malloc for the oauth load result (1-based),
 *   or -1 to disarm.
 *
 * Test-only hook for the PROVIDER_OAUTH_LOAD_OOM path. Only compiled with
 * -DSESSION_MANAGER_TEST=1.
 *
 * Return: void. Not thread-safe; call before any concurrent use.
 */
void session_manager_test_set_oauth_alloc_fail(int nth_allocation);
#endif

#endif
