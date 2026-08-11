/*
 * session_branch_info.c - branch_info assembly: chain counting for a
 * fork group and the session_manager_branch_info_alloc JSON builder.
 * Depends on: cJSON, session_manager, session_branch (created_at).
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "session_branch_info.h"
#include "session_branch.h"
#include "session_manager.h"
#include "session.h"
#include "../utils/string_utils.h"

static int branch_group_chain_count(cJSON *list, const char *group,
                                    char **created_ats, int max_created,
                                    char **ids, int max_ids)
{
    int count = 1; /* the live chain */
    int created_count = 0;
    int n = cJSON_GetArraySize(list);
    for (int i = 0; i < n && created_count < max_created; i++)
    {
        cJSON *rec = cJSON_GetArrayItem(list, i);
        cJSON *rec_msgs = rec ? cJSON_GetObjectItem(rec, "messages") : NULL;
        if (!rec_msgs || !cJSON_IsArray(rec_msgs)) continue;
        int mcount = cJSON_GetArraySize(rec_msgs);
        int hit = 0;
        for (int j = 0; j < mcount; j++)
        {
            cJSON *m = cJSON_GetArrayItem(rec_msgs, j);
            cJSON *g = m ? cJSON_GetObjectItem(m, "fork_group_id") : NULL;
            if (g && g->valuestring && strcmp(g->valuestring, group) == 0)
             {
                hit = 1;
                break;
            }
        }
        if (!hit) continue;
        count++;
        cJSON *created = cJSON_GetObjectItem(rec, "created_at");
        if (created && created->valuestring && created_ats)
            created_ats[created_count] = str_dup(created->valuestring);
        if (ids && max_ids > created_count)
        {
            cJSON *rid = cJSON_GetObjectItem(rec, "id");
            ids[created_count] =
                (rid && rid->valuestring) ? str_dup(rid->valuestring) : NULL;
        }
        created_count++;
    }
    return count;
}

char *session_manager_branch_info_alloc(SessionManager *sm, const char *session_id)
{
    if (!sm || !session_id || !sm->key_initialized || !sm->db) return NULL;

    session_manager_lock(sm);
    Session *s = session_manager_load_session_nolock_alloc(sm, session_id);
    if (!s) {
        session_manager_unlock(sm);
        return NULL;
    }

    char *result = NULL;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) goto cleanup;

    cJSON *branches = cJSON_GetObjectItem(s->metadata, "branches");
    cJSON *list = branches ? cJSON_GetObjectItem(branches, "list") : NULL;

    for (int i = 0; i < s->messages_count; i++)
    {
        if (!s->messages[i].fork_group_id) continue;

        char *created_ats[64];
        char *branch_ids[64];
        for (int k = 0; k < 64; k++)
        {
            created_ats[k] = NULL;
            branch_ids[k] = NULL;
        }
        int count = branch_group_chain_count(list, s->messages[i].fork_group_id,
                                             created_ats, 64,
                                             branch_ids, 64);
        if (count <= 0) { count = 1; }

        char *live_created = branch_active_created_at(s);
        int active = 1;
        if (live_created)
        {
            for (int k = 0; k < count - 1; k++)
            {
                if (created_ats[k] &&
                    strcmp(created_ats[k], live_created) < 0)
                    active++;
            }
        }

        cJSON *entry = cJSON_CreateObject();
        if (!entry)
        {
            free(live_created);
            for (int k = 0; k < 64; k++)
            {
                free(created_ats[k]);
                free(branch_ids[k]);
            }
            goto cleanup;
        }
        cJSON_AddStringToObject(entry, "message_id",
                                s->messages[i].id ? s->messages[i].id : "");
        cJSON_AddNumberToObject(entry, "count", count);
        cJSON_AddNumberToObject(entry, "active", active);
        /* Record ids of the non-live chains, ordered by chain creation —
         * the frontend's switch target is branch_ids[target-1] (the live
         * chain occupies `active` and has no record). */
        cJSON *ids_arr = cJSON_CreateArray();
        if (ids_arr)
        {
            for (int k = 0; k < count - 1; k++)
            {
                if (branch_ids[k])
                {
                    cJSON *sid = cJSON_CreateString(branch_ids[k]);
                    if (sid) cJSON_AddItemToArray(ids_arr, sid);
                }
            }
            cJSON_AddItemToObject(entry, "branch_ids", ids_arr);
        }
        if (!cJSON_AddItemToArray(arr, entry))
        {
            cJSON_Delete(entry);
            free(live_created);
            for (int k = 0; k < 64; k++)
            {
                free(created_ats[k]);
                free(branch_ids[k]);
            }
            goto cleanup;
        }
        free(live_created);
        for (int k = 0; k < 64; k++)
        {
            free(created_ats[k]);
            free(branch_ids[k]);
        }
    }

    result = cJSON_PrintUnformatted(arr);

cleanup:
    if (arr) cJSON_Delete(arr);
    session_free(s);
    session_manager_unlock(sm);
    return result;
}
