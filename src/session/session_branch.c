/*
 * session_branch.c - session branching subsystem: fork/switch/tag over
 * the snapshot-record data model in session->metadata. Branch_info
 * assembly lives in session_branch_info.c.
 * (see BRANCHING_IMPLEMENTATION_PLAN.md §2). Split out of session_manager.c.
 * Depends on: session_manager (lock + nolock load/save), session, message,
 * cjson, utils (string_utils).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cjson/cJSON.h>

#include "session_branch.h"
#include "session_manager.h"
#include "session.h"
#include "../agent/message.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"

#ifdef SESSION_MANAGER_TEST
/* Shared fault-injection hook: session_manager.c defines sm_test_strdup
 * (non-static under SESSION_MANAGER_TEST) backed by the module-wide str_dup
 * counter, so branch-code str_dups are intercepted by
 * session_manager_test_set_alloc_fail() exactly as before this file was
 * split out. Production builds never see this TU compiled with the define. */
char *sm_test_strdup(const char *s);
#define str_dup sm_test_strdup

/* The fork-id mints allocate via asprintf, which the str_dup counter
 * cannot reach — and failing them lands on the goto-cleanup path BEFORE
 * message_copy initializes fork_copy (the L5 window). The shim lives in
 * session_manager.c (shared by migration.c under the same counter);
 * this TU routes its asprintf calls through it. Position 1 is the
 * fork-message id mint, position 2 the fork-group mint, in call order. */
int sm_test_asprintf(char **strp, const char *fmt, ...);
#define asprintf sm_test_asprintf
#endif

/* ---------------------------------------------------------------------------
 * Branch support (fork / switch / branch_info)
 * ---------------------------------------------------------------------------
 *
 * Data model (see BRANCHING_IMPLEMENTATION_PLAN.md §2): session->messages
 * holds the active chain; every other chain lives as a snapshot record in
 * session->metadata.branches.list:
 *
 *   "branches": {
 *     "active_created_at": "<chain creation ts of the live chain>",
 *     "list": [ { "id": "br_<ts>-<rand>", "created_at": "...",
 *                 "anchor_message_id": "<id of last fork message>",
 *                 "messages": [ ... ] } ]
 *   }
 *
 * At fork time ONE fork_group_id is minted and shared by both the pre-fork
 * message (which keeps its id/content and gets the group only when it had
 * none) and the fresh fork message (new id, new group, new content), so the
 * branch pill renders on both chains and "chains containing a message with
 * that fork_group_id" counts correctly. Re-forking at a message that
 * already carries a fork_group_id keeps the old group on the pre-fork
 * message (per plan: "the pre-fork message keeps its old fork_group_id")
 * and mints a fresh group for the new message — a new fork node.
 *
 * Records are popped on switch-to (only non-live chains are stored), so a
 * chain never has a record while it is live; fork/switch-away therefore
 * usually append a new record. The anchor match is kept as a defensive
 * replace path for invariants that change in future.
 */

static char *branch_mint_id(const char *prefix)
{
    time_t now = time(NULL);
    char *id = NULL;
    if (asprintf(&id, "%s%ld-%d", prefix, (long)now, rand() % 1000000) < 0)
        return NULL;
    return id;
}

static char *branch_now_iso(void)
{
    char ts[64];
    struct timespec now;
    struct tm tm_storage;
    struct tm *tm_ptr;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return NULL;
    tm_ptr = localtime_r(&now.tv_sec, &tm_storage);
    if (!tm_ptr) return NULL;
    /* Millisecond resolution: with second-granularity timestamps a fork's
     * snapshot record and the new live chain can share a created_at, and
     * branch_info's strcmp ordering then reports the wrong active
     * position. Same-ms collisions are still possible but need real
     * timing luck rather than being the normal outcome of a fast edit. */
    if (strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm_ptr) == 0)
        return NULL;

    char *iso = NULL;
    if (asprintf(&iso, "%s.%03ld", ts, now.tv_nsec / 1000000L) < 0)
        return NULL;
    return iso;
}

/* Returns the cJSON "list" array of branch records, creating it (plus the
 * enclosing "branches" object) when absent, or NULL on OOM. */
static cJSON *branch_list_ensure(Session *s)
{
    cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
    if (!branches)
    {
        branches = cJSON_CreateObject();
        if (!branches) return NULL;
        if (!cJSON_AddItemToObject(s->metadata, "branches", branches))
        {
            cJSON_Delete(branches);
            return NULL;
        }
    }
    cJSON *list = cJSON_GetObjectItem(branches, "list");
    if (!list)
    {
        list = cJSON_CreateArray();
        if (!list) return NULL;
        if (!cJSON_AddItemToObject(branches, "list", list))
        {
            cJSON_Delete(list);
            return NULL;
        }
    }
    return list;
}

/* The live chain's creation timestamp: metadata.branches.active_created_at,
 * falling back to the session creation time for never-forked sessions.
 * Returns NULL only on OOM. */
char *branch_active_created_at(Session *s)
{
    cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
    cJSON *active = branches ? cJSON_GetObjectItem(branches, "active_created_at") : NULL;
    if (active && active->valuestring)
        return str_dup(active->valuestring);
    return str_dup(s->created_at ? s->created_at : "");
}

/* Chain-birth timestamp for a freshly born chain: the real time, bumped
 * past the previous live chain's birth when they'd share a millisecond.
 * A re-fork within the same ms as the previous fork otherwise yields a
 * tied birth, and branch_info's strict < ordering then drops the
 * re-forked chain from the active count (refork test: active=2 instead
 * of 3). The collision window is one millisecond, so waiting until the
 * real clock is strictly past the previous birth settles it — bounded
 * wait, since prev is at most a couple of ms ahead of the clock that
 * produced it. */
static char *branch_next_created_at(Session *s)
{
    char *now = branch_now_iso();
    if (!now) return NULL;
    char *prev = branch_active_created_at(s);
    if (!prev) {
        free(now);
        return NULL;
    }

    if (strcmp(now, prev) <= 0)
    {
        struct timespec pause = {0, 2000000L};
        for (int i = 0; i < 16 && strcmp(now, prev) <= 0; i++)
        {
            if (nanosleep(&pause, NULL) != 0) break;
            free(now);
            now = branch_now_iso();
            if (!now) {
                free(prev);
                return NULL;
            }
        }
    }
    free(prev);
    return now;
}

/* Sets branches.active_created_at to `value`, replacing an existing value.
 * A chain is re-born on every fork/switch, so the key must be updated in
 * place — cJSON_AddStringToObject on an existing key APPENDS a duplicate,
 * and GetObjectItem then reads the stale first one (branch_info ordering
 * breaks). */
static int branch_set_active_created_at(Session *s, const char *value)
{
    cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
    if (!branches) return -1;
    cJSON *act = cJSON_GetObjectItem(branches, "active_created_at");
    if (act)
    {
        cJSON *stale = cJSON_DetachItemFromObject(branches,
                                                  "active_created_at");
        if (stale) cJSON_Delete(stale);
    }
    return cJSON_AddStringToObject(branches, "active_created_at", value) ? 0 : -1;
}

/* Deep copy of the live chain as a cJSON array for a snapshot record. */
static cJSON *branch_snapshot_messages(Message *msgs, int count)
{
    return messages_to_json_array(msgs, count);
}

/* Creates a snapshot record object for the live chain. old_chain_created_at
 * is the chain's true creation time (NOT the snapshot time — ordering of
 * branch_info depends on it). anchor_message_id may be NULL. */
static cJSON *branch_record_create(const char *created_at,
                                   const char *anchor_message_id,
                                   cJSON *snapshot)
{
    cJSON *rec = cJSON_CreateObject();
    if (!rec) return NULL;
    char *id = branch_mint_id("br_");
    if (!id) {
        cJSON_Delete(rec);
        return NULL;
    }
    if (!cJSON_AddStringToObject(rec, "id", id)) {
        free(id);
        cJSON_Delete(rec);
        return NULL;
    }
    free(id);
    if (!cJSON_AddStringToObject(rec, "created_at", created_at ? created_at : ""))
     {
        cJSON_Delete(rec);
        return NULL;
    }
    if (anchor_message_id)
    {
        if (!cJSON_AddStringToObject(rec, "anchor_message_id", anchor_message_id))
         {
            cJSON_Delete(rec);
            return NULL;
        }
    }
    if (!cJSON_AddItemToObject(rec, "messages", snapshot))
     {
        cJSON_Delete(rec);
        return NULL;
    }
    return rec;
}

/* Appends a snapshot record for the live chain to branches.list. The
 * record's created_at is the chain's own creation time (NOT the snapshot
 * time — branch_info orders chains by creation), so a chain that left
 * live keeps its original position in the ordering. A chain is recorded
 * fresh on every leave-live transition; switch_branch pops the live
 * chain's own record first, so appending can never duplicate a chain
 * that is still in the list. IMPORTANT: records are NEVER replaced by
 * anchor match — sibling chains of the same fork share the fork point's
 * id, and replacing by anchor destroyed sibling data (switch-back bug). */
static int branch_record_snapshot_live(Session *s, cJSON *list)
{
    char *created_at = branch_active_created_at(s);
    if (!created_at) return -1;

    cJSON *snapshot = branch_snapshot_messages(s->messages, s->messages_count);
    if (!snapshot) {
        free(created_at);
        return -1;
    }

    cJSON *rec = branch_record_create(created_at, NULL, snapshot);
    free(created_at);
    if (!rec) {
        cJSON_Delete(snapshot);
        return -1;
    }
    if (!cJSON_AddItemToArray(list, rec))
    {
        cJSON_Delete(rec);
        return -1;
    }
    return 0;
}

/* Finds the record with `branch_id` in `list`, or NULL. */
static cJSON *branch_record_find(cJSON *list, const char *branch_id)
{
    if (!list || !branch_id) return NULL;
    int n = cJSON_GetArraySize(list);
    for (int i = 0; i < n; i++)
    {
        cJSON *rec = cJSON_GetArrayItem(list, i);
        cJSON *rid = rec ? cJSON_GetObjectItem(rec, "id") : NULL;
        if (rid && rid->valuestring && strcmp(rid->valuestring, branch_id) == 0)
            return rec;
    }
    return NULL;
}

/* Frees every message in [index, count) and shrinks the array. */
static void session_truncate_messages(Session *s, int index)
{
    for (int i = index; i < s->messages_count; i++)
        message_clear(&s->messages[i]);
    s->messages_count = index;
}

/* Resolves the fork point to a message index: by message id when given,
 * else by the raw index. Returns -1 when the resolved index is out of
 * range. */
static int branch_find_fork_index(Session *s, const char *message_id, int index)
{
    int fi = -1;
    if (message_id)
    {
        for (int i = 0; i < s->messages_count; i++)
            if (s->messages[i].id && strcmp(s->messages[i].id, message_id) == 0)
             {
                fi = i;
                break;
            }
    }
    if (fi < 0)
        fi = index;
    if (fi < 0 || fi >= s->messages_count)
        return -1;
    return fi;
}


/* Performs the fork once the fork point index is known: mints ids,
 * snapshots the pre-fork chain, truncates the live array, re-births the
 * chain, persists, and fills *out. The session must be loaded and the
 * manager lock held. Returns 0 on success, -1 on any failure (the
 * session on disk is untouched on failure — all-or-nothing per plan
 * §3.2). */
static int branch_do_fork(SessionManager *sm, Session *s, int fi,
                          const char *new_content,
                          SessionManagerForkResult *out)
{
    int rc = -1;
    cJSON *list = NULL;
    char *created_at = NULL;
    char *fork_message_id = NULL;
    char *fork_group_id = NULL;
    /* Zeroed so the OOM goto-cleanup paths below can message_clear it
     * safely before message_copy ever initializes it (frees of garbage
     * pointers were the historical bug here). */
    Message fork_copy = {0};

    list = branch_list_ensure(s);
    if (!list) goto cleanup;

    /* Mint an id for the fork message. The group is shared with the
     * pre-fork message — and when the fork point already carries a group
     * (re-forking a previous fork point, e.g. re-editing the same user
     * message), the new chain JOINS that group instead of minting a new
     * one: a fresh group would orphan the pre-fork chain from the pill
     * (its chains carry the old group) and the count would not span the
     * whole family. */
    fork_message_id = branch_mint_id("m_");
    if (!fork_message_id) goto cleanup;
    if (s->messages[fi].fork_group_id)
        fork_group_id = str_dup(s->messages[fi].fork_group_id);
    else
        fork_group_id = branch_mint_id("fg_");
    if (!fork_group_id) goto cleanup;

    /* Copy the pre-fork message; the copy gets the fresh identity and
     * content, the original keeps its content in the snapshot. */
    if (message_copy(&fork_copy, &s->messages[fi]) != 0) goto cleanup;
    free(fork_copy.id);
    fork_copy.id = str_dup(fork_message_id);
    free(fork_copy.fork_group_id);
    fork_copy.fork_group_id = str_dup(fork_group_id);
    if (new_content)
    {
        char *content_dup = str_dup(new_content);
        if (!content_dup) goto cleanup;
        free(fork_copy.content);
        fork_copy.content = content_dup;
    }
    if (!fork_copy.id || !fork_copy.fork_group_id) goto cleanup;

    /* Pre-fork message keeps its identity; give it the minted group (and an
     * id when legacy) so the pill renders on the old chain too. This MUST
     * happen before the snapshot below — branch_info counts chains by the
     * fork_group_id present in each chain, so the old chain's snapshot has
     * to carry the shared group or it would not count (pill would show
     * count=1 and switch-back would lose the pill entirely). */
    if (!s->messages[fi].fork_group_id)
    {
        char *group_dup = str_dup(fork_group_id);
        if (!group_dup) goto cleanup;
        s->messages[fi].fork_group_id = group_dup;
    }
    if (!s->messages[fi].id)
    {
        char *id_dup = str_dup(fork_message_id);
        if (!id_dup) goto cleanup;
        s->messages[fi].id = id_dup;
    }

    /* Snapshot the full pre-fork chain — nothing may be truncated or
     * committed on disk until this exists (all-or-nothing per plan §3.2;
     * on any failure below the session is untouched on disk). */
    cJSON *snapshot = branch_snapshot_messages(s->messages, s->messages_count);
    if (!snapshot) goto cleanup;
    created_at = branch_active_created_at(s);
    if (!created_at) {
        cJSON_Delete(snapshot);
        goto cleanup;
    }
    cJSON *rec = branch_record_create(created_at, NULL, snapshot);
    if (!rec) {
        cJSON_Delete(snapshot);
        goto cleanup;
    }
    if (!cJSON_AddItemToArray(list, rec))
    {
        cJSON_Delete(rec);
        goto cleanup;
    }

    /* Truncate the live array at the fork point and swap in the fork copy.
     * The pre-fork message is only needed for the snapshot above — clear it
     * or its fields (including the group/id assigned pre-snapshot) leak. */
    session_truncate_messages(s, fi + 1);
    message_clear(&s->messages[fi]);
    s->messages[fi] = fork_copy;
    memset(&fork_copy, 0, sizeof(fork_copy));

    /* The new live chain is born now — strictly past the previous birth
     * so same-ms re-forks keep a deterministic branch_info ordering. */
    char *now = branch_next_created_at(s);
    if (!now) goto cleanup;
    cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
    if (!branches) {
        free(now);
        goto cleanup;
    }
    if (branch_set_active_created_at(s, now) != 0)
     {
        free(now);
        goto cleanup;
    }
    free(now);

    if (session_manager_save_session_nolock(sm, s) != 0) goto cleanup;

    out->branch_id = str_dup(cJSON_GetObjectItem(rec, "id")->valuestring);
    out->fork_message_id = str_dup(fork_message_id);
    out->fork_group_id = str_dup(fork_group_id);
    if (!out->branch_id || !out->fork_message_id || !out->fork_group_id)
    {
        /* Nothing was persisted atomically — roll the caller's view back
         * to "no fork happened". */
        memset(out, 0, sizeof(*out));
        goto cleanup;
    }
    /* fork_copy was moved into the live array and zeroed; hand the caller
     * its own deep copy of the new tail. Failure here mirrors the str_dup
     * rollback above: the fork IS committed on disk but the caller sees
     * -1 with *out zeroed (reload renders the fork). */
    if (message_copy(&out->fork_message, &s->messages[fi]) != 0)
    {
        memset(out, 0, sizeof(*out));
        goto cleanup;
    }
    rc = 0;

cleanup:
    message_clear(&fork_copy);
    free(created_at);
    free(fork_message_id);
    free(fork_group_id);
    return rc;
}


int session_manager_fork_branch(SessionManager *sm, const char *session_id,
                               const char *message_id, int index,
                               const char *new_content,
                               SessionManagerForkResult *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!sm || !session_id || !sm->key_initialized || !sm->db || !out)
        return -1;

    session_manager_lock(sm);
    Session *s = session_manager_load_session_nolock_alloc(sm, session_id);
    if (!s)
    {
        session_manager_unlock(sm);
        log_error("fork_branch: session load failed",
                  "session_id", session_id, NULL);
        return -1;
    }

    int fi = branch_find_fork_index(s, message_id, index);
    if (fi < 0)
    {
        session_free(s);
        session_manager_unlock(sm);
        log_error("fork_branch: fork point not found",
                  "session_id", session_id, "index", index, NULL);
        return -1;
    }

    int rc = branch_do_fork(sm, s, fi, new_content, out);
    session_free(s);
    session_manager_unlock(sm);
    if (rc != 0)
    {
        log_error("fork_branch: fork failed",
                  "session_id", session_id, "index", index, NULL);
    }
    return rc;
}

int session_manager_switch_branch(SessionManager *sm, const char *session_id,
                                  const char *branch_id)
{
    if (!sm || !session_id || !branch_id || !sm->key_initialized || !sm->db)
        return -1;

    session_manager_lock(sm);
    Session *s = session_manager_load_session_nolock_alloc(sm, session_id);
    if (!s) {
        session_manager_unlock(sm);
        return -1;
    }

    int rc = -1;
    cJSON *list = branch_list_ensure(s);
    if (!list) goto cleanup;
    cJSON *rec = branch_record_find(list, branch_id);
    if (!rec) goto cleanup;

    /* Serialize the target snapshot once — used both for the no-op
     * comparison below and for loading. */
    cJSON *snap = cJSON_GetObjectItem(rec, "messages");
    if (!snap || !cJSON_IsArray(snap)) goto cleanup;
    char *snap_str = cJSON_PrintUnformatted(snap);
    if (!snap_str) goto cleanup;

    /* A switch to the chain that is already live is a no-op — recording
     * it would append a phantom duplicate record and inflate the pill
     * count. Compare serialized message arrays. */
    cJSON *live_json = messages_to_json_array(s->messages, s->messages_count);
    if (!live_json) {
        free(snap_str);
        goto cleanup;
    }
    char *live_str = cJSON_PrintUnformatted(live_json);
    cJSON_Delete(live_json);
    if (!live_str) {
        free(snap_str);
        goto cleanup;
    }
    int same = strcmp(live_str, snap_str) == 0;
    free(live_str);
    if (same)
    {
        rc = 0;
        goto cleanup;
    }

    /* Preserve the current live chain so no chain is lost. */
    if (branch_record_snapshot_live(s, list) != 0)
     {
        free(snap_str);
        goto cleanup;
    }

    /* Load the target snapshot into session->messages. */
    session_truncate_messages(s, 0);
    int drc = session_deserialize_messages(s, snap_str);
    free(snap_str);
    if (drc != 0) goto cleanup;

    /* The newly live chain's creation time is its record's; legacy
     * records without one get a fresh birth strictly past the previous
     * live chain's (same-ms collision would misorder branch_info). */
    cJSON *rec_created = cJSON_GetObjectItem(rec, "created_at");
    if (rec_created && rec_created->valuestring)
    {
        cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
        if (!branches) goto cleanup;
        if (branch_set_active_created_at(s, rec_created->valuestring) != 0)
            goto cleanup;
    }
    else
    {
        char *now = branch_next_created_at(s);
        if (!now) goto cleanup;
        cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
        if (!branches) {
            free(now);
            goto cleanup;
        }
        if (branch_set_active_created_at(s, now) != 0)
         {
            free(now);
            goto cleanup;
        }
        free(now);
    }

    /* Pop the target record — only non-live chains are stored. */
    int n = cJSON_GetArraySize(list);
    for (int i = 0; i < n; i++)
    {
        if (cJSON_GetArrayItem(list, i) == rec)
        {
            cJSON_DetachItemFromArray(list, i);
            cJSON_Delete(rec);
            break;
        }
    }

    if (session_manager_save_session_nolock(sm, s) != 0) goto cleanup;
    rc = 0;

cleanup:
    session_free(s);
    session_manager_unlock(sm);
    return rc;
}

/* Tags the message at `index` of the live chain with `fork_group_id` plus
 * a freshly minted id — marks the regenerated assistant response as the
 * new chain's fork point. The fork copy minted by fork_branch is dropped
 * when the agent context is saved over the live chain after the run, so
 * the marker is re-applied here. All-or-nothing: on any failure the
 * session is unchanged on disk and NULL is returned. Caller frees. */
char *session_manager_tag_message_new(SessionManager *sm, const char *session_id,
                                  int index, const char *fork_group_id)
{
    if (!sm || !session_id || !fork_group_id || !sm->key_initialized || !sm->db)
        return NULL;

    session_manager_lock(sm);
    Session *s = session_manager_load_session_nolock_alloc(sm, session_id);
    if (!s) {
        session_manager_unlock(sm);
        return NULL;
    }

    int rc = -1;
    char *id_out = NULL;
    if (index < 0 || index >= s->messages_count) goto cleanup;

    char *id = branch_mint_id("m_");
    if (!id) goto cleanup;

    Message *m = &s->messages[index];
    if (!m->fork_group_id)
    {
        char *group = str_dup(fork_group_id);
        if (!group) {
            free(id);
            goto cleanup;
        }
        m->fork_group_id = group;
    }
    free(m->id);
    m->id = id;

    id_out = str_dup(m->id);
    if (!id_out) goto cleanup;
    if (session_manager_save_session_nolock(sm, s) != 0) goto cleanup;
    rc = 0;

cleanup:
    if (rc != 0)
    {
        free(id_out);
        id_out = NULL;
    }
    session_free(s);
    session_manager_unlock(sm);
    return id_out;
}
