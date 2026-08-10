/*
 * change_tracker.h - undo/redo stacks that snapshot file contents before
 * edits and restore them on demand.
 * Depends on: <stddef.h>.
 */

#ifndef ECHO_CHANGE_TRACKER_H
#define ECHO_CHANGE_TRACKER_H

#include <stddef.h>

#define CT_MAX_STACK 64

typedef struct {
    char *file_path;
    char *previous_content;
    size_t content_len;
} CTEntry;

typedef struct {
    CTEntry undo_stack[CT_MAX_STACK];
    int undo_count;
    CTEntry redo_stack[CT_MAX_STACK];
    int redo_count;
} ChangeTracker;

/**
 * ct_create - allocate an empty change tracker
 *
 * Return: caller-owned ChangeTracker (free with ct_destroy()), or NULL on
 * allocation failure. Thread-safe; no shared state.
 */
ChangeTracker *ct_create(void);

/**
 * ct_destroy - free a tracker and every stack entry
 * @ct: tracker to release, or NULL (no-op).
 *
 * Frees the file_path and previous_content of every undo and redo entry,
 * then the tracker itself.
 *
 * Return: void. Thread-safe; no shared state.
 */
void ct_destroy(ChangeTracker *ct);

/**
 * ct_snapshot - record a file's current contents on the undo stack
 * @ct: tracker to use; must be non-NULL.
 * @file_path: path of the file to read, borrowed for the duration of the
 *   call; the tracker keeps its own copy.
 *
 * Reads the whole file into a new undo entry (the file is not modified)
 * and clears the redo stack — a new snapshot invalidates the redo
 * history. When the undo stack is full the oldest entry is dropped.
 *
 * Return: 0 on success, -1 when ct or file_path is NULL, the file cannot
 * be opened or read, or an allocation fails. Thread-safe with respect to
 * other trackers; one tracker must not be used concurrently.
 */
int ct_snapshot(ChangeTracker *ct, const char *file_path);

/**
 * ct_undo - restore a file to its most recent snapshot
 * @ct: tracker to use; must be non-NULL.
 *
 * Restores the file to the top undo entry's previous_content (recreating
 * it if it was deleted) and pushes the current file content onto the redo
 * stack; the undo entry is then popped. When the current file cannot be
 * read, the redo entry stores no content and a later ct_redo() truncates
 * the file to zero bytes. On allocation failure the file is already
 * restored but the undo entry is retained and the pre-undo content is
 * lost.
 *
 * Return: number of bytes written to the file, or -1 when ct is NULL, the
 * undo stack is empty, the file cannot be opened for writing, or an
 * allocation fails. Thread-safe with respect to other trackers; one
 * tracker must not be used concurrently.
 */
int ct_undo(ChangeTracker *ct);

/**
 * ct_redo - re-apply the most recently undone change
 * @ct: tracker to use; must be non-NULL.
 *
 * Writes the top redo entry's content back to the file and moves the
 * entry to the undo stack — ownership of the entry's strings transfers,
 * nothing is copied.
 *
 * Return: number of bytes written, or -1 when ct is NULL, the redo stack
 * is empty, or the file cannot be opened for writing. Thread-safe with
 * respect to other trackers; one tracker must not be used concurrently.
 */
int ct_redo(ChangeTracker *ct);

#ifdef CHANGE_TRACKER_TEST
/**
 * change_tracker_test_set_alloc_fail - fail the Nth str_dup call
 * @nth_allocation: 1-based index of the str_dup call to make fail, or -1
 *   to disable failure injection.
 *
 * Test-only hook: makes the Nth str_dup inside change_tracker.c return
 * NULL so allocation-failure paths can be exercised deterministically.
 * Only compiled under CHANGE_TRACKER_TEST.
 *
 * Return: void.
 */
void change_tracker_test_set_alloc_fail(int nth_allocation);
#endif

#endif