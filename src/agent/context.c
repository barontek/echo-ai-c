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
    double priority;
} MsgScore;

static int score_cmp(const void *a, const void *b)
{
    double diff = ((const MsgScore *)b)->priority - ((const MsgScore *)a)->priority;
    if (diff > 0) return 1;
    if (diff < 0) return -1;
    return 0;
}

Message *smart_select(Message *msgs, int count, int keep_count)
{
    if (count <= keep_count)
    {
        Message *copy = calloc(count, sizeof(Message));
        if (!copy) return NULL;
        memcpy(copy, msgs, count * sizeof(Message));
        return copy;
    }

    MsgScore *scores = calloc(count, sizeof(MsgScore));
    if (!scores) return NULL;

    for (int i = 0; i < count; i++)
    {
        scores[i].orig_index = i;
        if (strcmp(msgs[i].role, "system") == 0)
            scores[i].priority = 100.0;
        else if (strcmp(msgs[i].role, "tool") == 0)
            scores[i].priority = 50.0;
        else
            scores[i].priority = (double)i / count;
    }

    qsort(scores, count, sizeof(MsgScore), score_cmp);

    Message *selected = calloc(keep_count, sizeof(Message));
    if (!selected) { free(scores); return NULL; }

    for (int i = 0; i < keep_count; i++)
        selected[i] = msgs[scores[i].orig_index];

    free(scores);
    return selected;
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

    int char_budget = (int)(max_chars * 0.7);
    int msg_budget = max_messages;

    Message *trimmed = smart_select(msgs, *count, msg_budget);
    if (!trimmed) return msgs;

    int total = 0;
    for (int i = 0; total < *count && i < msg_budget; i++)
    {
        if (trimmed[i].content)
        {
            int clen = (int)strlen(trimmed[i].content);
            if (total + clen > char_budget)
            {
                trimmed[i].content[char_budget - total] = '\0';
                total = char_budget;
                break;
            }
            total += clen;
        }
    }

    free(msgs);
    *count = msg_budget;
    return trimmed;
}
