#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "message.h"
#include "../utils/string_utils.h"

char *split_thinking_content(const char *raw)
{
    if (!raw) return NULL;

    const char *start = raw;
    const char *think_end = NULL;

    while (1)
    {
        const char *open = strstr(start, "<think>");
        if (!open) break;

        const char *close = strstr(open, "</think>");
        if (!close)
        {
            open = NULL;
            break;
        }

        think_end = close + 8;
        start = think_end;
    }

    if (!think_end) return str_dup(raw);

    size_t len = (think_end - raw);
    char *result = malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, raw, len);
    result[len] = '\0';
    return result;
}

typedef struct {
    int orig_index;
    int priority_group;
    double score;
} MsgScore;

static int score_cmp(const void *a, const void *b)
{
    const MsgScore *sa = (const MsgScore *)a;
    const MsgScore *sb = (const MsgScore *)b;
    if (sa->priority_group != sb->priority_group)
        return sa->priority_group - sb->priority_group;
    if (sb->score > sa->score) return 1;
    if (sb->score < sa->score) return -1;
    return 0;
}

static int is_tool_role(const char *role)
{
    return role && strcmp(role, "tool") == 0;
}

static int is_assistant_role(const char *role)
{
    return role && strcmp(role, "assistant") == 0;
}

Message *smart_select(Message *msgs, int count, int keep_count)
{
    if (count <= keep_count)
    {
        Message *copy = calloc(count, sizeof(Message));
        if (!copy) return NULL;
        for (int i = 0; i < count; i++)
        {
            if (message_copy(&copy[i], &msgs[i]) != 0)
            {
                message_free_all(copy, count);
                return NULL;
            }
        }
        return copy;
    }

    MsgScore *scores = calloc(count, sizeof(MsgScore));
    if (!scores) return NULL;

    /* Priority 1: system messages */
    /* Priority 2: tool messages (keep tool+assistant pairs) */
    /* Priority 3: recent messages (40% of budget) */
    /* Priority 4: evenly sampled remainder */

    int recent_budget = (int)(keep_count * 0.4);
    if (recent_budget < 1) recent_budget = 1;
    for (int i = 0; i < count; i++)
    {
        scores[i].orig_index = i;

        if (strcmp(msgs[i].role, "system") == 0)
        {
            scores[i].priority_group = 1;
            scores[i].score = 0;
        }
        else if (is_tool_role(msgs[i].role))
        {
            scores[i].priority_group = 2;
            scores[i].score = (double)i;
        }
        else
        {
            int recent_start = count - recent_budget;
            if (i >= recent_start)
            {
                scores[i].priority_group = 3;
                scores[i].score = (double)(i - recent_start);
            }
            else
            {
                scores[i].priority_group = 4;
                scores[i].score = (double)i;
            }
        }
    }

    qsort(scores, count, sizeof(MsgScore), score_cmp);

    int *selected_flags = calloc(count, sizeof(int));
    if (!selected_flags) { free(scores); return NULL; }

    int selected_count = 0;
    int result_count = 0;

    /* pass 1: select up to keep_count */
    for (int i = 0; i < count && selected_count < keep_count; i++)
    {
        int idx = scores[i].orig_index;
        selected_flags[idx] = 1;
        selected_count++;

        /* if this is a tool message, also keep the preceding assistant */
        if (is_tool_role(msgs[idx].role) && idx > 0 && !selected_flags[idx - 1] &&
            is_assistant_role(msgs[idx - 1].role))
        {
            selected_flags[idx - 1] = 1;
            selected_count++;
        }
    }

    /* pass 2: ensure tool results aren't orphaned — find unmatched assistants */
    for (int i = 1; i < count; i++)
    {
        if (is_tool_role(msgs[i].role) && selected_flags[i] && !selected_flags[i - 1] &&
            is_assistant_role(msgs[i - 1].role))
        {
            selected_flags[i - 1] = 1;
        }
        /* if assistant is kept but tool result is not, also keep the tool */
        if (i + 1 < count && is_tool_role(msgs[i + 1].role) &&
            selected_flags[i] && !selected_flags[i + 1] &&
            is_assistant_role(msgs[i].role))
        {
            selected_flags[i + 1] = 1;
        }
    }

    /* build result */
    result_count = 0;
    for (int i = 0; i < count; i++)
    {
        if (selected_flags[i]) result_count++;
    }

    if (result_count > keep_count)
    {
        result_count = keep_count;
        int dropped = 0;
        for (int i = 0; i < count && dropped < result_count; i++)
        {
            if (!selected_flags[i]) continue;
            if (dropped >= result_count) selected_flags[i] = 0;
            else dropped++;
        }
    }

    if (result_count <= 0) { free(selected_flags); free(scores); return NULL; }

    Message *selected = calloc(result_count, sizeof(Message));
    if (!selected) { free(selected_flags); free(scores); return NULL; }

    int out = 0;
    for (int i = 0; i < count && out < result_count; i++)
    {
        if (selected_flags[i])
        {
            if (message_copy(&selected[out], &msgs[i]) != 0)
            {
                message_free_all(selected, result_count);
                free(selected_flags);
                free(scores);
                return NULL;
            }
            out++;
        }
    }

    free(selected_flags);
    free(scores);

    if (out < result_count)
    {
        Message *trimmed = realloc(selected, sizeof(Message) * out);
        if (trimmed) selected = trimmed;
    }

    return selected;
}

/* Rough token estimate: 1 token ~ 4 chars */
static int estimate_tokens(const char *text)
{
    if (!text) return 0;
    return (int)(strlen(text) / 4) + 1;
}

Message *trim_messages_by_tokens(Message *msgs, int *count, int max_tokens)
{
    if (!msgs || !count || *count == 0) return msgs;

    int total = 0;
    for (int i = 0; i < *count; i++)
        total += estimate_tokens(msgs[i].content);

    if (total <= max_tokens) return msgs;

    /* reverse-iterate, drop oldest messages first */
    /* keep at least the first system message and the most recent user+assistant+tool exchange */

    int *keep = calloc(*count, sizeof(int));
    if (!keep) return msgs;

    /* always keep system messages */
    for (int i = 0; i < *count; i++)
    {
        if (strcmp(msgs[i].role, "system") == 0)
            keep[i] = 1;
    }

    /* keep the most recent messages */
    int budget = max_tokens;
    for (int i = *count - 1; i >= 0; i--)
    {
        if (keep[i]) continue;
        int t = estimate_tokens(msgs[i].content);
        if (budget - t >= 0)
        {
            keep[i] = 1;
            budget -= t;
        }
        else
        {
            break;
        }
    }

    /* ensure tool results aren't orphaned */
    for (int i = 1; i < *count; i++)
    {
        if (is_tool_role(msgs[i].role) && keep[i] && !keep[i - 1] &&
            is_assistant_role(msgs[i - 1].role))
            keep[i - 1] = 1;
        if (i + 1 < *count && is_tool_role(msgs[i + 1].role) &&
            keep[i] && !keep[i + 1] && is_assistant_role(msgs[i].role))
            keep[i + 1] = 1;
    }

    int new_count = 0;
    for (int i = 0; i < *count; i++)
        if (keep[i]) new_count++;

    Message *trimmed = calloc(new_count, sizeof(Message));
    if (!trimmed) { free(keep); return msgs; }

    int out = 0;
    for (int i = 0; i < *count; i++)
    {
        if (keep[i])
        {
            trimmed[out++] = msgs[i];
            memset(&msgs[i], 0, sizeof(msgs[i]));
        }
    }

    free(keep);
    message_free_all(msgs, *count);
    *count = new_count;
    return trimmed;
}

Message *apply_context_window(Message *msgs, int *count,
                              int max_messages, int max_chars)
{
    if (*count <= max_messages)
    {
        int total_chars = 0;
        for (int i = 0; i < *count; i++)
        {
            if (msgs[i].content)
                total_chars += strlen(msgs[i].content);
        }
        if (total_chars <= max_chars) return msgs;
    }

    /* trim by messages first using smart_select */
    int msg_budget = max_messages;
    Message *selected = smart_select(msgs, *count, msg_budget);
    if (!selected) return msgs;

    /* then trim by token budget */
    int approx_tokens = max_chars / 4;
    if (approx_tokens < 1) approx_tokens = 1;
    int new_count = *count < msg_budget ? *count : msg_budget;
    Message *result = trim_messages_by_tokens(selected, &new_count, approx_tokens);

    *count = new_count;
    return result;
}
