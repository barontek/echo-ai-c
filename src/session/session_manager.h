#ifndef ECHO_SESSION_MANAGER_H
#define ECHO_SESSION_MANAGER_H

#include <sqlite3.h>
#include <pthread.h>
#include <stdatomic.h>
#include "session.h"
#include "encryption.h"

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

SessionManager *session_manager_create(const char *data_dir, const char *password);
/* Returns an owned manager and classifies authentication versus storage failure. */
SessionManager *session_manager_create_ex(const char *data_dir,
                                          const char *password,
                                          SessionManagerCreateResult *result);
/* Retains a shared manager; each successful retain requires one free call. */
SessionManager *session_manager_retain(SessionManager *sm);
/* Releases one owned reference and destroys the manager after the last one. */
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

/* Result of a fork: the old chain's snapshot record id plus the minted
 * identity of the new fork message. All strings are caller-owned (free
 * individually); fork_message is a deep copy the caller owns (message_clear
 * it after use). */
typedef struct {
    char *branch_id;
    char *fork_message_id;
    char *fork_group_id;
    Message fork_message;
} SessionManagerForkResult;

/* Forks the live chain of `session_id` at the message resolved from
 * `message_id` (scan for id match) or, when NULL/not found, `index`
 * (bounds-checked). The pre-fork chain is deep-copied into a snapshot
 * record under metadata.branches.list (never mutated afterwards); the live
 * array is truncated after the fork point; the fork message is replaced by
 * a deep copy with freshly minted id + fork_group_id and (when non-NULL)
 * new content; the pre-fork message at the fork point shares the same
 * minted fork_group_id (and gets an id minted if it lacked one) so the
 * pill renders on both chains. All-or-nothing: any allocation failure
 * leaves the session unchanged on disk and returns -1 with *out zeroed.
 * On success returns 0 and fills *out. */
int session_manager_fork_branch(SessionManager *sm, const char *session_id,
                                const char *message_id, int index,
                                const char *new_content,
                                SessionManagerForkResult *out);

/* Switches the live chain of `session_id` to the snapshot stored under
 * `branch_id`. The current live chain is first snapshotted (a fresh record
 * is appended — records are never replaced by anchor match, since sibling
 * chains share the fork point's id), so no chain is ever lost. Switching
 * to the chain that is already live is a no-op. Returns 0 on success, -1
 * on unknown branch / load / allocation failure (session unchanged). */
int session_manager_switch_branch(SessionManager *sm, const char *session_id,
                                  const char *branch_id);

/* Tags the message at `index` of the live chain of `session_id` with
 * `fork_group_id` plus a freshly minted id, marking it as the chain's fork
 * point (used for the regenerated assistant response after a regenerate
 * fork). A message that already carries a fork_group_id keeps it. Returns
 * the minted message id (caller frees) on success, NULL on failure —
 * all-or-nothing, session unchanged on disk on failure. */
char *session_manager_tag_message(SessionManager *sm, const char *session_id,
                                  int index, const char *fork_group_id);

/* Builds the branch_info JSON array for the live chain of `session_id`:
 * [{message_id, count, active}] — one entry per fork point on the live
 * chain (message carrying a fork_group_id). count = number of chains
 * (snapshot records + live) containing a message with that fork_group_id;
 * active = 1-based position of the live chain among them, ordered by chain
 * creation time. Returns a caller-freed string, or NULL on OOM/error.
 * Sessions without metadata.branches yield an empty array. */
char *session_manager_branch_info_alloc(SessionManager *sm, const char *session_id);
char *session_manager_export_session(SessionManager *sm, const char *session_id);
Session *session_manager_import_session(SessionManager *sm, const char *json_str);
int session_manager_purge_sessions(SessionManager *sm, int older_than_days);
int session_manager_log_event(SessionManager *sm, const char *session_id,
                               const char *event_type, const char *data);

/* Saves encrypted provider credentials under provider_name. The caller retains
 * ownership of data; returns -1 on invalid input, encryption, SQL, or OOM. */
int session_manager_save_provider_oauth(SessionManager *sm,
                                        const char *provider_name,
                                        const char *data);

/* Loads credentials into caller-owned *data_out; failure leaves it NULL. */
ProviderOAuthLoadResult session_manager_load_provider_oauth_ex(
    SessionManager *sm, const char *provider_name, char **data_out);

/* Compatibility loader returning caller-owned data, or NULL for any failure. */
char *session_manager_load_provider_oauth(SessionManager *sm,
                                           const char *provider_name);

/* Deletes stored provider credentials and returns 0 on success, -1 on error. */
int session_manager_delete_provider_oauth(SessionManager *sm,
                                          const char *provider_name);

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
void session_manager_test_set_realloc_fail(int nth_allocation);
void session_manager_test_set_bind_fail(int nth_bind);
void session_manager_test_set_encrypt_fail(int nth_encrypt);
void session_manager_test_set_oauth_alloc_fail(int nth_allocation);
#endif

#endif
