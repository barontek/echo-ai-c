/*
 * agent_summarize.c - context-window summarization: transcript
 * assembly and a summarization LLM call into context_summary.
 * Depends on: message, logging, string_utils.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "agent_internal.h"
#include "agent_summarize.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"


int agent_perform_summarization(Agent *agent, int original_count)
{
    (void)original_count;
    if (!agent || !agent->provider) return -1;

    /* C13: accumulate in size_t with an overflow flag — the old int
     * accumulator was UB past ~2 GB of message content and fed
     * malloc(text_len + 1). The skip semantics are preserved: a
     * transcript larger than 2x the context budget is not summarized. */
    int text_over = 0;
    size_t text_len = 0;
    size_t summary_cap = (size_t)agent->max_context_chars * 2;
    for (int i = 0; i < agent->messages_count; i++)
    {
        if (agent->messages[i].content)
        {
            size_t clen = strlen(agent->messages[i].content);
            if (text_len > summary_cap - clen)
            {
                text_over = 1;
                break;
            }
            text_len += clen;
        }
    }

    if (text_over || text_len > summary_cap) return -1;

    char *text = malloc(text_len + 1);
    if (!text)
    {
        log_error("agent_perform_summarization: OOM allocating transcript", NULL);
        return -1;
    }
    text[0] = '\0';
    for (int i = 0; i < agent->messages_count; i++)
    {
        /* Buffer is exactly sized (sum of content lengths + 1), so a
         * truncating append would mean the size computation drifted —
         * bail out instead of summarizing a silently partial transcript. */
        if (agent->messages[i].content &&
            strlcat(text, agent->messages[i].content, (size_t)text_len + 1U) >= (size_t)text_len + 1U)
        {
            free(text);
            return -1;
        }
    }

    Message sum_msgs[2];
    memset(sum_msgs, 0, sizeof(sum_msgs));
    sum_msgs[0].role = str_dup("system");
    sum_msgs[0].content = str_dup("Summarize this conversation concisely in 2-3 sentences.");

    char *truncated = text;
    if (text_len > 4000)
    {
        truncated[4000] = '\0';
    }    sum_msgs[1].role = str_dup("user");
    sum_msgs[1].content = str_dup(truncated);

    LLMResponse *resp = agent->provider->chat(
        agent->provider, sum_msgs, 2,
        agent->model, 0.3, 30, NULL);

    free(sum_msgs[0].role);
    free(sum_msgs[0].content);
    free(sum_msgs[1].role);
    free(sum_msgs[1].content);
    free(text);

    if (resp && resp->content)
    {
        char *trimmed = str_trim(str_dup(resp->content));
        llm_response_free(resp);
        if (!trimmed)
        {
            log_error("agent_perform_summarization: OOM trimming summary", NULL);
            return -1;
        }
        free(agent->context_summary);
        agent->context_summary = trimmed;
        return 0;
    }
    if (resp) llm_response_free(resp);
    return -1;
}
