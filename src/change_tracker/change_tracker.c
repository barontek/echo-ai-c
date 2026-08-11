/*
 * change_tracker.c - undo/redo stacks that snapshot file contents before
 * edits and restore them on demand.
 * Depends on: string_utils, stdio file I/O.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "change_tracker.h"
#include "../utils/string_utils.h"

#ifdef CHANGE_TRACKER_TEST
static int ct_alloc_counter = 0;
static int ct_alloc_fail_at = -1;

void change_tracker_test_set_alloc_fail(int nth_allocation)
{
    ct_alloc_counter = 0;
    ct_alloc_fail_at = nth_allocation;
}

static char *ct_test_strdup(const char *s)
{
    ct_alloc_counter++;
    if (ct_alloc_counter == ct_alloc_fail_at) return NULL;
    return str_dup(s);
}

#define str_dup ct_test_strdup
#endif

ChangeTracker *ct_create(void)
{
    return calloc(1, sizeof(ChangeTracker));
}

void ct_destroy(ChangeTracker *ct)
{
    if (!ct) return;
    for (int i = 0; i < ct->undo_count; i++)
    {
        free(ct->undo_stack[i].file_path);
        free(ct->undo_stack[i].previous_content);
    }
    for (int i = 0; i < ct->redo_count; i++)
    {
        free(ct->redo_stack[i].file_path);
        free(ct->redo_stack[i].previous_content);
    }
    free(ct);
}

int ct_snapshot(ChangeTracker *ct, const char *file_path)
{
    if (!ct || !file_path) return -1;

    FILE *f = fopen(file_path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len < 0) {
        fclose(f);
        return -1;
    }

    char *content = malloc((size_t)len + 1);
    if (!content) {
        fclose(f);
        return -1;
    }

    size_t read = fread(content, 1, (size_t)len, f);
    fclose(f);
    content[read] = '\0';

    if (ct->undo_count >= CT_MAX_STACK)
    {
        free(ct->undo_stack[0].file_path);
        free(ct->undo_stack[0].previous_content);
        memmove(ct->undo_stack, ct->undo_stack + 1,
                (CT_MAX_STACK - 1) * sizeof(CTEntry));
        ct->undo_count--;
    }

    char *fp = str_dup(file_path);
    if (!fp) {
        free(content);
        return -1;
    }

    int idx = ct->undo_count++;
    ct->undo_stack[idx].file_path = fp;
    ct->undo_stack[idx].previous_content = content;
    ct->undo_stack[idx].content_len = (size_t)read;

    /* a new snapshot invalidates the redo history */
    for (int i = 0; i < ct->redo_count; i++)
    {
        free(ct->redo_stack[i].file_path);
        free(ct->redo_stack[i].previous_content);
    }
    ct->redo_count = 0;

    return 0;
}

int ct_undo(ChangeTracker *ct)
{
    if (!ct || ct->undo_count == 0) return -1;

    CTEntry *entry = &ct->undo_stack[ct->undo_count - 1];

    FILE *f = fopen(entry->file_path, "r");
    char *current = NULL;
    if (f)
    {
        fseek(f, 0, SEEK_END);
        long cur_len = ftell(f);
        rewind(f);
        if (cur_len > 0)
        {
            current = malloc((size_t)cur_len + 1);
            if (current)
            {
                size_t read = fread(current, 1, (size_t)cur_len, f);
                current[read] = '\0';
            }
        }
        fclose(f);
    }

    /* restore the file before pushing the redo entry: if the redo-push
     * allocation below fails, the file is already restored and the undo
     * entry is retained */
    f = fopen(entry->file_path, "w");
    if (!f)
    {
        free(current);
        return -1;
    }
    size_t written = 0;
    if (entry->content_len > 0)
        written = fwrite(entry->previous_content, 1, entry->content_len, f);
    if (written != entry->content_len || fclose(f) != 0)
    {
        /* C7: a failed restore must not consume the undo entry — it is
         * the only copy of the previous content. Keep the entry (the
         * file may be truncated; a retry can restore it) and report
         * failure. */
        free(current);
        return -1;
    }

    if (ct->redo_count >= CT_MAX_STACK)
    {
        free(ct->redo_stack[0].file_path);
        free(ct->redo_stack[0].previous_content);
        memmove(ct->redo_stack, ct->redo_stack + 1,
                (CT_MAX_STACK - 1) * sizeof(CTEntry));
        ct->redo_count--;
    }

    char *fp = str_dup(entry->file_path);
    if (!fp) {
        free(current);
        return -1;
    }

    int rdx = ct->redo_count++;
    ct->redo_stack[rdx].file_path = fp;
    ct->redo_stack[rdx].previous_content = current;
    ct->redo_stack[rdx].content_len = current ? strlen(current) : 0;

    free(entry->file_path);
    free(entry->previous_content);
    ct->undo_count--;

    return (int)written;
}

int ct_redo(ChangeTracker *ct)
{
    if (!ct || ct->redo_count == 0) return -1;

    CTEntry *entry = &ct->redo_stack[ct->redo_count - 1];

    FILE *f = fopen(entry->file_path, "w");
    if (!f) return -1;
    size_t written = 0;
    if (entry->content_len > 0)
        written = fwrite(entry->previous_content, 1, entry->content_len, f);
    if (written != entry->content_len || fclose(f) != 0)
    {
        /* C7 mirror: a failed redo must keep the redo entry intact. */
        return -1;
    }

    if (ct->undo_count >= CT_MAX_STACK)
    {
        free(ct->undo_stack[0].file_path);
        free(ct->undo_stack[0].previous_content);
        memmove(ct->undo_stack, ct->undo_stack + 1,
                (CT_MAX_STACK - 1) * sizeof(CTEntry));
        ct->undo_count--;
    }

    int ux = ct->undo_count++;
    ct->undo_stack[ux].file_path = entry->file_path;
    ct->undo_stack[ux].previous_content = entry->previous_content;
    ct->undo_stack[ux].content_len = entry->content_len;
    entry->file_path = NULL;
    entry->previous_content = NULL;
    ct->redo_count--;

    return (int)written;
}
