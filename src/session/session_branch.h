/*
 * session_branch.h - session branching: fork/switch/tag and branch_info
 * over the snapshot-record model. Implemented in session_branch.c.
 * Depends on: session_manager.h (SessionManager, lock/unlock,
 * load/save _nolock), message.h (Message).
 */

#ifndef ECHO_SESSION_BRANCH_H
#define ECHO_SESSION_BRANCH_H

#include "session_manager.h"
#include "../agent/message.h"

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
 * session_manager_tag_message_new - mark a message as a chain fork point
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
char *session_manager_tag_message_new(SessionManager *sm, const char *session_id,
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

#endif
