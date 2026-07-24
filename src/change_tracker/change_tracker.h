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

ChangeTracker *ct_create(void);
void ct_destroy(ChangeTracker *ct);
int ct_snapshot(ChangeTracker *ct, const char *file_path);
int ct_undo(ChangeTracker *ct);
int ct_redo(ChangeTracker *ct);

#ifdef CHANGE_TRACKER_TEST
void change_tracker_test_set_alloc_fail(int nth_allocation);
#endif

#endif