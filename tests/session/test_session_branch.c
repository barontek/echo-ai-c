/* test_session_branch.c - session store Branches tests
 * Split from test_session_manager.c (2026-08 file-length compliance);
 * shared fixtures live in test_session_fixture.c. Depends on: check,
 * the session store under SESSION_MANAGER_TEST.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "test_session_fixture.h"

static SessionManager *branch_sm_create(char *tmpdir, char **sid_out)
{
    SessionManager *sm = session_manager_create(tmpdir, "pw");
    ck_assert_ptr_nonnull(sm);
    Session *s = session_manager_create_session(sm, "branch test");
    ck_assert_ptr_nonnull(s);
    *sid_out = str_dup(s->id);
    ck_assert_ptr_nonnull(*sid_out);
    session_free(s);
    return sm;
}

static void branch_add_msgs(SessionManager *sm, const char *sid,
                            const char *role1, const char *c1,
                            const char *role2, const char *c2)
{
    ck_assert_int_eq(session_manager_add_message(sm, sid, role1, c1,
                                                 NULL, NULL), 0);
    if (role2)
        ck_assert_int_eq(session_manager_add_message(sm, sid, role2, c2,
                                                     NULL, NULL), 0);
}


static void branch_free_result(SessionManagerForkResult *r)
{
    free(r->branch_id);
    free(r->fork_message_id);
    free(r->fork_group_id);
    message_clear(&r->fork_message);
    memset(r, 0, sizeof(*r));
}

/* Finds the branch record with `id` in the loaded session's metadata, or
 * NULL. Caller must not free the returned cJSON (owned by s->metadata). */
static cJSON *branch_find_record(Session *s, const char *id)
{
    cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
    cJSON *list = branches ? cJSON_GetObjectItem(branches, "list") : NULL;
    if (!list) return NULL;
    int n = cJSON_GetArraySize(list);
    for (int i = 0; i < n; i++)
    {
        cJSON *rec = cJSON_GetArrayItem(list, i);
        cJSON *rid = rec ? cJSON_GetObjectItem(rec, "id") : NULL;
        if (rid && rid->valuestring && strcmp(rid->valuestring, id) == 0)
            return rec;
    }
    return NULL;
}

/* Finds the branch_info entry with the given message_id, or NULL. */
static cJSON *branch_info_entry(cJSON *info, const char *message_id)
{
    int n = cJSON_GetArraySize(info);
    for (int i = 0; i < n; i++)
    {
        cJSON *e = cJSON_GetArrayItem(info, i);
        cJSON *mid = e ? cJSON_GetObjectItem(e, "message_id") : NULL;
        if (mid && mid->valuestring && strcmp(mid->valuestring, message_id) == 0)
            return e;
    }
    return NULL;
}

START_TEST(test_fork_creates_branch_record)
{
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "q1", "assistant", "a1");

    SessionManagerForkResult out = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0,
                                                 "q1 edited", &out), 0);
    ck_assert_ptr_nonnull(out.branch_id);
    ck_assert_ptr_nonnull(out.fork_message_id);
    ck_assert_ptr_nonnull(out.fork_group_id);
    ck_assert_str_ne(out.branch_id, out.fork_message_id);
    /* the returned fork message is the new live chain's tail */
    ck_assert_str_eq(out.fork_message.content, "q1 edited");
    ck_assert_str_eq(out.fork_message.id, out.fork_message_id);
    ck_assert_str_eq(out.fork_message.fork_group_id, out.fork_group_id);

    /* live chain: truncated to the fork point, tail replaced by the fork */
    Session *v = session_manager_load_session_alloc(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_int_eq(v->messages_count, 1);
    ck_assert_str_eq(v->messages[0].content, "q1 edited");
    ck_assert_str_eq(v->messages[0].id, out.fork_message_id);
    ck_assert_str_eq(v->messages[0].fork_group_id, out.fork_group_id);

    /* snapshot record: original chain preserved, old tail intact, fork
     * point carries the SHARED group (so the old chain counts) */
    cJSON *rec = branch_find_record(v, out.branch_id);
    ck_assert_ptr_nonnull(rec);
    cJSON *rec_msgs = cJSON_GetObjectItem(rec, "messages");
    ck_assert_int_eq(cJSON_GetArraySize(rec_msgs), 2);
    cJSON *m0 = cJSON_GetArrayItem(rec_msgs, 0);
    ck_assert_str_eq(cJSON_GetObjectItem(m0, "content")->valuestring, "q1");
    ck_assert_str_eq(cJSON_GetObjectItem(m0, "fork_group_id")->valuestring,
                     out.fork_group_id);
    cJSON *m1 = cJSON_GetArrayItem(rec_msgs, 1);
    ck_assert_str_eq(cJSON_GetObjectItem(m1, "content")->valuestring, "a1");

    session_free(v);
    branch_free_result(&out);
    free(sid);
    session_manager_free(sm);
}
END_TEST

START_TEST(test_fork_by_message_id_resolves_index)
{
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "user", "u1");
    ck_assert_int_eq(session_manager_add_message(sm, sid, "assistant", "a2",
                                                 NULL, NULL), 0);

    /* give the messages stable ids so an id-anchored fork is possible */
    Session *s = session_manager_load_session_alloc(sm, sid);
    ck_assert_ptr_nonnull(s);
    ck_assert_int_eq(s->messages_count, 3);
    s->messages[0].id = str_dup("m-0");
    s->messages[1].id = str_dup("m-1");
    s->messages[2].id = str_dup("m-2");
    ck_assert_int_eq(session_manager_save_session(sm, s), 0);
    session_free(s);

    /* index 99 is out of bounds — id must win and cut at m-1 */
    SessionManagerForkResult out = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, "m-1", 99,
                                                 "u1 edited", &out), 0);
    Session *v = session_manager_load_session_alloc(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_int_eq(v->messages_count, 2);
    ck_assert_str_eq(v->messages[0].content, "u0");
    ck_assert_str_eq(v->messages[1].content, "u1 edited");
    ck_assert_str_eq(v->messages[1].id, out.fork_message_id);
    session_free(v);
    branch_free_result(&out);
    free(sid);
    session_manager_free(sm);
}
END_TEST

START_TEST(test_fork_unknown_message_id_falls_back_to_index)
{
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    SessionManagerForkResult out = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, "no-such-id", 0,
                                                 "edited", &out), 0);
    Session *v = session_manager_load_session_alloc(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_int_eq(v->messages_count, 1);
    ck_assert_str_eq(v->messages[0].content, "edited");
    session_free(v);
    branch_free_result(&out);
    free(sid);
    session_manager_free(sm);
}
END_TEST

START_TEST(test_regenerate_keeps_content)
{
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "q1", "assistant", "a1");

    /* content == NULL → the fork message keeps the original content */
    SessionManagerForkResult out = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, NULL,
                                                 &out), 0);
    ck_assert_str_eq(out.fork_message.content, "q1");
    Session *v = session_manager_load_session_alloc(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_int_eq(v->messages_count, 1);
    ck_assert_str_eq(v->messages[0].content, "q1");
    ck_assert_str_eq(v->messages[0].id, out.fork_message_id);
    session_free(v);
    branch_free_result(&out);
    free(sid);
    session_manager_free(sm);
}
END_TEST

START_TEST(test_switch_branch_swaps_live_and_preserves_chains)
{
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    /* chain births are strictly monotonic: branch_now_iso() stamps
     * millisecond timestamps and branch_next_created_at() bump-waits past
     * any tie, so ordering is deterministic without sleeping (this was
     * historically ensured with sleep(1), pre-3af9f68) */
    SessionManagerForkResult f1 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "e1",
                                                 &f1), 0);
    SessionManagerForkResult f2 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "e2",
                                                 &f2), 0);

    /* switch back to the original chain (record br f1.branch_id) */
    ck_assert_int_eq(session_manager_switch_branch(sm, sid, f1.branch_id), 0);
    Session *v = session_manager_load_session_alloc(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_int_eq(v->messages_count, 2);
    ck_assert_str_eq(v->messages[0].content, "u0");
    ck_assert_str_eq(v->messages[1].content, "a1");
    /* the switched-to record is popped (no chain is live while recorded) */
    ck_assert_ptr_null(branch_find_record(v, f1.branch_id));
    cJSON *branches = cJSON_GetObjectItem(v->metadata, "branches");
    cJSON *list = branches ? cJSON_GetObjectItem(branches, "list") : NULL;
    ck_assert_ptr_nonnull(list);
    ck_assert_int_eq(cJSON_GetArraySize(list), 2);
    /* the pre-switch live chain was snapshotted (preservation), and the
     * re-activated chain still counts its fork node: the pill must show
     * 1/3, not 1/1 (regression for the snapshot-before-group bug). Count
     * is 3 because f2 re-forked the f1 fork point (e1 carries fg1) and so
     * JOINED fg1 — one pill entry covering all three chains, never two */
    char *info = session_manager_branch_info_alloc(sm, sid);
    ck_assert_ptr_nonnull(info);
    cJSON *info_json = cJSON_Parse(info);
    free(info);
    ck_assert_ptr_nonnull(info_json);
    ck_assert_int_eq(cJSON_GetArraySize(info_json), 1);
    cJSON *e = branch_info_entry(info_json, f1.fork_message_id);
    ck_assert_ptr_nonnull(e);
    ck_assert_int_eq(cJSON_GetObjectItem(e, "count")->valueint, 3);
    ck_assert_int_eq(cJSON_GetObjectItem(e, "active")->valueint, 1);
    cJSON_Delete(info_json);
    session_free(v);

    /* switch to the middle chain (e1) — live chain again swaps */
    ck_assert_int_eq(session_manager_switch_branch(sm, sid, f2.branch_id), 0);
    Session *v2 = session_manager_load_session_alloc(sm, sid);
    ck_assert_ptr_nonnull(v2);
    ck_assert_int_eq(v2->messages_count, 1);
    ck_assert_str_eq(v2->messages[0].content, "e1");
    ck_assert_str_eq(v2->messages[0].id, f1.fork_message_id);
    session_free(v2);

    branch_free_result(&f1);
    branch_free_result(&f2);
    free(sid);
    session_manager_free(sm);
}
END_TEST

START_TEST(test_branch_info_counts_multifork_chain)
{
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");
    ck_assert_int_eq(session_manager_add_message(sm, sid, "user", "u2",
                                                 NULL, NULL), 0);
    ck_assert_int_eq(session_manager_add_message(sm, sid, "assistant", "a3",
                                                 NULL, NULL), 0);

    SessionManagerForkResult f1 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "e1",
                                                 &f1), 0);
    /* continue the forked chain, then fork a second point deeper in */
    ck_assert_int_eq(session_manager_add_message(sm, sid, "user", "u2b",
                                                 NULL, NULL), 0);
    ck_assert_int_eq(session_manager_add_message(sm, sid, "assistant", "a3b",
                                                 NULL, NULL), 0);
    SessionManagerForkResult f2 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 2, "e2",
                                                 &f2), 0);

    char *info = session_manager_branch_info_alloc(sm, sid);
    ck_assert_ptr_nonnull(info);
    cJSON *info_json = cJSON_Parse(info);
    free(info);
    ck_assert_ptr_nonnull(info_json);
    ck_assert_int_eq(cJSON_GetArraySize(info_json), 2);

    /* first fork point: live + original snapshot + first-fork snapshot */
    cJSON *e1 = branch_info_entry(info_json, f1.fork_message_id);
    ck_assert_ptr_nonnull(e1);
    ck_assert_int_eq(cJSON_GetObjectItem(e1, "count")->valueint, 3);
    ck_assert_int_eq(cJSON_GetObjectItem(e1, "active")->valueint, 3);
    /* both non-live chains carry records, ordered by creation */
    cJSON *e1_ids = cJSON_GetObjectItem(e1, "branch_ids");
    ck_assert_ptr_nonnull(e1_ids);
    ck_assert_int_eq(cJSON_GetArraySize(e1_ids), 2);
    ck_assert_str_eq(cJSON_GetArrayItem(e1_ids, 0)->valuestring,
                     f1.branch_id);

    /* second fork point: live + second-fork snapshot (original chain does
     * not contain it) */
    cJSON *e2 = branch_info_entry(info_json, f2.fork_message_id);
    ck_assert_ptr_nonnull(e2);
    ck_assert_int_eq(cJSON_GetObjectItem(e2, "count")->valueint, 2);
    ck_assert_int_eq(cJSON_GetObjectItem(e2, "active")->valueint, 2);
    cJSON *e2_ids = cJSON_GetObjectItem(e2, "branch_ids");
    ck_assert_ptr_nonnull(e2_ids);
    ck_assert_int_eq(cJSON_GetArraySize(e2_ids), 1);
    ck_assert_str_eq(cJSON_GetArrayItem(e2_ids, 0)->valuestring,
                     f2.branch_id);
    cJSON_Delete(info_json);

    branch_free_result(&f1);
    branch_free_result(&f2);
    free(sid);
    session_manager_free(sm);
}
END_TEST

START_TEST(test_old_session_without_branches_reports_empty)
{
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    char *info = session_manager_branch_info_alloc(sm, sid);
    ck_assert_ptr_nonnull(info);
    ck_assert_str_eq(info, "[]");
    free(info);

    free(sid);
    session_manager_free(sm);
}
END_TEST

START_TEST(test_fork_allocation_failure_leaves_session_unchanged)
{
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "q1", "assistant", "a1");

    /* Fail the 5th str_dup: load_session_locked does 4 (id, empty title,
     * created_at, decrypted title) and the fork path's first is
     * branch_active_created_at — before the snapshot/truncate/commit, so
     * nothing may change on disk (all-or-nothing per plan §3.2). */
    SessionManagerForkResult out = {0};
    session_manager_test_set_alloc_fail(5);
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "edited",
                                                 &out), -1);
    session_manager_test_set_alloc_fail(-1);
    /* *out zeroed, no dangling pointers */
    ck_assert_ptr_null(out.branch_id);
    ck_assert_ptr_null(out.fork_message_id);
    ck_assert_ptr_null(out.fork_group_id);
    ck_assert_ptr_null(out.fork_message.content);

    /* session unchanged on disk: original chain, no groups, no branches */
    Session *v = session_manager_load_session_alloc(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_int_eq(v->messages_count, 2);
    ck_assert_str_eq(v->messages[0].content, "q1");
    ck_assert_str_eq(v->messages[1].content, "a1");
    ck_assert_ptr_null(v->messages[0].fork_group_id);
    ck_assert_ptr_null(cJSON_GetObjectItem(v->metadata, "branches"));
    session_free(v);

    /* normal operation restored after the fault injection is lifted */
    SessionManagerForkResult out2 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "edited",
                                                 &out2), 0);
    Session *v2 = session_manager_load_session_alloc(sm, sid);
    ck_assert_ptr_nonnull(v2);
    ck_assert_int_eq(v2->messages_count, 1);
    ck_assert_str_eq(v2->messages[0].content, "edited");
    session_free(v2);

    branch_free_result(&out2);
    free(sid);
    session_manager_free(sm);
}
END_TEST

START_TEST(test_fork_early_allocation_failures_are_clean)
{
    for (int fail_at = 1; fail_at <= 4; fail_at++)
    {
        char *sid = NULL;
        SessionManager *sm = branch_sm_create(tmpdir, &sid);
        branch_add_msgs(sm, sid, "user", "q1", "assistant", "a1");
        SessionManagerForkResult out = {0};
        session_manager_test_set_alloc_fail(fail_at);
        ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0,
                                                     "edited", &out), -1);
        session_manager_test_set_alloc_fail(-1);
        ck_assert_ptr_null(out.branch_id);
        ck_assert_ptr_null(out.fork_message_id);
        ck_assert_ptr_null(out.fork_group_id);
        ck_assert_ptr_null(out.fork_message.content);
        Session *v = session_manager_load_session_alloc(sm, sid);
        ck_assert_ptr_nonnull(v);
        ck_assert_int_eq(v->messages_count, 2);
        session_free(v);
        free(sid);
        session_manager_free(sm);
    }
}
END_TEST

START_TEST(test_fork_mint_allocation_failures_are_clean)
{
    for (int fail_at = 1; fail_at <= 2; fail_at++)
    {
        char *sid = NULL;
        SessionManager *sm = branch_sm_create(tmpdir, &sid);
        branch_add_msgs(sm, sid, "user", "q1", "assistant", "a1");
        SessionManagerForkResult out = {0};
        session_manager_test_set_asprintf_fail(fail_at);
        ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0,
                                                     "edited", &out), -1);
        session_manager_test_set_asprintf_fail(-1);
        ck_assert_ptr_null(out.branch_id);
        ck_assert_ptr_null(out.fork_message_id);
        ck_assert_ptr_null(out.fork_group_id);
        ck_assert_ptr_null(out.fork_message.content);
        /* disk untouched: still the original two-message chain */
        Session *v = session_manager_load_session_alloc(sm, sid);
        ck_assert_ptr_nonnull(v);
        ck_assert_int_eq(v->messages_count, 2);
        ck_assert_str_eq(v->messages[1].content, "a1");
        session_free(v);
        free(sid);
        session_manager_free(sm);
    }
}
END_TEST

START_TEST(test_switch_away_and_back_preserves_branch_data)
{
    /* Regression for the switch-back data-loss bug (found via live smoke):
     * branch_record_snapshot_live used to REPLACE the record whose
     * anchor_message_id matched the live chain's last fork id — and
     * sibling chains of one fork share the fork point's id, so switching
     * back clobbered the record of the chain the user had just left
     * (history reverted to the unedited content, pill collapsed to
     * count=1). Records must be appended, never replaced by anchor. */
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    SessionManagerForkResult f1 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "edited",
                                                 &f1), 0);

    /* switch to the old chain */
    ck_assert_int_eq(session_manager_switch_branch(sm, sid, f1.branch_id), 0);
    Session *v1 = session_manager_load_session_alloc(sm, sid);
    ck_assert_ptr_nonnull(v1);
    ck_assert_int_eq(v1->messages_count, 2);
    ck_assert_str_eq(v1->messages[0].content, "u0");
    session_free(v1);

    /* the edited chain must still exist as a record, and the pill count
     * must still span both chains */
    char *info1 = session_manager_branch_info_alloc(sm, sid);
    ck_assert_ptr_nonnull(info1);
    cJSON *j1 = cJSON_Parse(info1);
    free(info1);
    ck_assert_ptr_nonnull(j1);
    ck_assert_int_eq(cJSON_GetArraySize(j1), 1);
    cJSON *e1 = branch_info_entry(j1, f1.fork_message_id);
    ck_assert_ptr_nonnull(e1);
    ck_assert_int_eq(cJSON_GetObjectItem(e1, "count")->valueint, 2);
    ck_assert_int_eq(cJSON_GetObjectItem(e1, "active")->valueint, 1);
    cJSON *e1_ids = cJSON_GetObjectItem(e1, "branch_ids");
    ck_assert_ptr_nonnull(e1_ids);
    ck_assert_int_eq(cJSON_GetArraySize(e1_ids), 1);
    char *other_id = str_dup(cJSON_GetArrayItem(e1_ids, 0)->valuestring);
    ck_assert_ptr_nonnull(other_id);
    cJSON_Delete(j1);

    /* switch back: the edited content must survive */
    ck_assert_int_eq(session_manager_switch_branch(sm, sid, other_id), 0);
    free(other_id);
    Session *v2 = session_manager_load_session_alloc(sm, sid);
    ck_assert_ptr_nonnull(v2);
    ck_assert_int_eq(v2->messages_count, 1);
    ck_assert_str_eq(v2->messages[0].content, "edited");

    char *info2 = session_manager_branch_info_alloc(sm, sid);
    ck_assert_ptr_nonnull(info2);
    cJSON *j2 = cJSON_Parse(info2);
    free(info2);
    ck_assert_ptr_nonnull(j2);
    cJSON *e2 = branch_info_entry(j2, f1.fork_message_id);
    ck_assert_ptr_nonnull(e2);
    ck_assert_int_eq(cJSON_GetObjectItem(e2, "count")->valueint, 2);
    ck_assert_int_eq(cJSON_GetObjectItem(e2, "active")->valueint, 2);
    cJSON_Delete(j2);
    session_free(v2);

    branch_free_result(&f1);
    free(sid);
    session_manager_free(sm);
}
END_TEST

START_TEST(test_refork_joins_existing_group)
{
    /* Re-editing the same message must JOIN the fork point's existing
     * fork_group_id: a fresh group would orphan the earlier fork's chains
     * from the pill (its records carry the old group) and the count would
     * not span the whole family. */
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    SessionManagerForkResult f1 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "e1",
                                                 &f1), 0);
    /* the fork point now carries f1's group — a re-fork joins it */
    SessionManagerForkResult f2 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "e2",
                                                 &f2), 0);
    ck_assert_str_eq(f2.fork_group_id, f1.fork_group_id);

    char *info = session_manager_branch_info_alloc(sm, sid);
    ck_assert_ptr_nonnull(info);
    cJSON *info_json = cJSON_Parse(info);
    free(info);
    ck_assert_ptr_nonnull(info_json);
    /* exactly one pill entry spans all three chains */
    ck_assert_int_eq(cJSON_GetArraySize(info_json), 1);
    cJSON *e = branch_info_entry(info_json, f2.fork_message_id);
    ck_assert_ptr_nonnull(e);
    ck_assert_int_eq(cJSON_GetObjectItem(e, "count")->valueint, 3);
    ck_assert_int_eq(cJSON_GetObjectItem(e, "active")->valueint, 3);
    cJSON_Delete(info_json);

    branch_free_result(&f1);
    branch_free_result(&f2);
    free(sid);
    session_manager_free(sm);
}
END_TEST

START_TEST(test_fork_same_second_orders_active)
{
    /* Millisecond-granularity chain births: with second-granularity
     * timestamps a fork right after session creation produced active=1
     * (the record's created_at and the live chain's shared one second and
     * strcmp ordering won). Deliberately NO sleep here — the fork happens
     * milliseconds after session creation, which the old code misordered
     * and the ms-resolution code orders deterministically. */
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    SessionManagerForkResult f1 = {0};
    ck_assert_int_eq(session_manager_fork_branch(sm, sid, NULL, 0, "e1",
                                                 &f1), 0);

    char *info = session_manager_branch_info_alloc(sm, sid);
    ck_assert_ptr_nonnull(info);
    cJSON *info_json = cJSON_Parse(info);
    free(info);
    ck_assert_ptr_nonnull(info_json);
    cJSON *e = branch_info_entry(info_json, f1.fork_message_id);
    ck_assert_ptr_nonnull(e);
    ck_assert_int_eq(cJSON_GetObjectItem(e, "count")->valueint, 2);
    ck_assert_int_eq(cJSON_GetObjectItem(e, "active")->valueint, 2);
    cJSON_Delete(info_json);

    branch_free_result(&f1);
    free(sid);
    session_manager_free(sm);
}
END_TEST

START_TEST(test_tag_message_marks_fork_point)
{
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    char *tagged = session_manager_tag_message_new(sm, sid, 1, "fg_tag");
    ck_assert_ptr_nonnull(tagged);
    ck_assert_str_ne(tagged, "");

    Session *v = session_manager_load_session_alloc(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_int_eq(v->messages_count, 2);
    ck_assert_str_eq(v->messages[1].role, "assistant");
    ck_assert_str_eq(v->messages[1].id, tagged);
    ck_assert_str_eq(v->messages[1].fork_group_id, "fg_tag");
    /* the user message is untouched */
    ck_assert_ptr_null(v->messages[0].id);
    ck_assert_ptr_null(v->messages[0].fork_group_id);
    char *old_id = str_dup(tagged);
    ck_assert_ptr_nonnull(old_id);
    session_free(v);
    free(tagged);

    /* re-tagging a message that already carries a group keeps the group */
    char *tagged2 = session_manager_tag_message_new(sm, sid, 1, "fg_other");
    ck_assert_ptr_nonnull(tagged2);
    Session *v2 = session_manager_load_session_alloc(sm, sid);
    ck_assert_ptr_nonnull(v2);
    ck_assert_str_eq(v2->messages[1].fork_group_id, "fg_tag");
    ck_assert_str_ne(v2->messages[1].id, old_id);
    session_free(v2);
    free(old_id);
    free(tagged2);

    /* out-of-range index: NULL, nothing changes */
    ck_assert_ptr_null(session_manager_tag_message_new(sm, sid, 5, "fg_x"));
    ck_assert_ptr_null(session_manager_tag_message_new(sm, sid, -1, "fg_x"));

    free(sid);
    session_manager_free(sm);
}
END_TEST

START_TEST(test_tag_message_oom_leaves_session_unchanged)
{
    char *sid = NULL;
    SessionManager *sm = branch_sm_create(tmpdir, &sid);
    branch_add_msgs(sm, sid, "user", "u0", "assistant", "a1");

    /* The 5th str_dup: load_session_locked does 4 (id, empty title,
     * created_at, decrypted title) and the group dup is tag_message's
     * first — it must fail BEFORE anything is committed. */
    session_manager_test_set_alloc_fail(5);
    ck_assert_ptr_null(session_manager_tag_message_new(sm, sid, 1, "fg_tag"));
    session_manager_test_set_alloc_fail(-1);

    Session *v = session_manager_load_session_alloc(sm, sid);
    ck_assert_ptr_nonnull(v);
    ck_assert_ptr_null(v->messages[1].id);
    ck_assert_ptr_null(v->messages[1].fork_group_id);
    session_free(v);

    /* normal operation restored after the fault injection is lifted */
    char *tagged = session_manager_tag_message_new(sm, sid, 1, "fg_tag");
    ck_assert_ptr_nonnull(tagged);
    free(tagged);

    free(sid);
    session_manager_free(sm);
}
END_TEST


Suite *session_mgr_branch_suite(void)
{
    Suite *s = suite_create("SessionManagerBranch");

    TCase *tc = tcase_create("Branches");
    tcase_add_checked_fixture(tc, tmpdir_setup, tmpdir_teardown);
    tcase_set_timeout(tc, 180);
    tcase_add_test(tc, test_fork_creates_branch_record);
    tcase_add_test(tc, test_fork_by_message_id_resolves_index);
    tcase_add_test(tc, test_fork_unknown_message_id_falls_back_to_index);
    tcase_add_test(tc, test_regenerate_keeps_content);
    tcase_add_test(tc, test_switch_branch_swaps_live_and_preserves_chains);
    tcase_add_test(tc, test_branch_info_counts_multifork_chain);
    tcase_add_test(tc, test_old_session_without_branches_reports_empty);
    tcase_add_test(tc, test_fork_allocation_failure_leaves_session_unchanged);
    tcase_add_test(tc, test_fork_early_allocation_failures_are_clean);
    tcase_add_test(tc, test_fork_mint_allocation_failures_are_clean);
    tcase_add_test(tc, test_switch_away_and_back_preserves_branch_data);
    tcase_add_test(tc, test_refork_joins_existing_group);
    tcase_add_test(tc, test_fork_same_second_orders_active);
    tcase_add_test(tc, test_tag_message_marks_fork_point);
    tcase_add_test(tc, test_tag_message_oom_leaves_session_unchanged);

    suite_add_tcase(s, tc);

    return s;
}

int main(void)
{
    Suite *s = session_mgr_branch_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
