/*
 * agent_title.c - title generation: model call, think-tag
 * stripping, quote trimming, fallback, and persistence.
 * Depends on: session_manager, message, string_utils.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "agent_internal.h"
#include "agent_title.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"


static char *strip_think_tags(const char *str)
{
    if (!str) return NULL;

    const char *open_tag  = "<think>";
    const char *close_tag = "</think>";
    size_t open_len  = strlen(open_tag);
    size_t close_len = strlen(close_tag);

    const char *open = strstr(str, open_tag);
    if (!open) return str_dup(str);

    const char *close = strstr(open + open_len, close_tag);
    if (!close) return str_dup(str);

    size_t before     = (size_t)(open - str);
    size_t suffix_off = (size_t)(close + close_len - str);
    size_t after      = strlen(str) - suffix_off;
    size_t result_len = before + after;

    char *result = malloc(result_len + 1);
    if (!result) return NULL;
    if (before > 0) memcpy(result, str, before);
    if (after  > 0) memcpy(result + before, str + suffix_off, after);
    result[result_len] = '\0';
    return result;
}

static char *agent_title_from_model(LLMProvider *provider, const char *model,
                                    const char *first_user_msg,
                                    const char *fallback)
{
    char *prompt = NULL;
    if (asprintf(&prompt,
                 "Summarize the following user request into a very short, "
                 "descriptive title (max 5 words). "
                 "Do not use quotes or a period.\n\n"
                 "User request: %s",
                 first_user_msg) < 0)
    {
        return NULL;
    }

    log_info("title prompt", "text", prompt, NULL);

    Message title_msg;
    memset(&title_msg, 0, sizeof(title_msg));
    title_msg.role    = str_dup("user");
    title_msg.content = prompt;
    if (!title_msg.role)
    {
        free(prompt);
        return NULL;
    }

    LLMResponse *resp = provider->chat(
        provider, &title_msg, 1,
        model, 0.3, 30, NULL);

    free(title_msg.role);
    free(title_msg.content);

    char *final_title = NULL;

    if (resp && resp->content)
    {
        log_info("title from model", "title", resp->content, NULL);

        char *raw = str_dup(resp->content);
        llm_response_free(resp);

        if (raw)
        {
            char *t = str_trim(raw);
            if (t && t[0])
            {
                char *no_think = strip_think_tags(t);
                if (no_think)
                {
                    char *c = str_trim(no_think);
                    if (c && c[0])
                    {
                        /* strip leading / trailing double-quotes */
                        size_t clen = strlen(c);
                        if ((c[0] == '"' && c[clen - 1] == '"')
                            || (c[0] == '\'' && c[clen - 1] == '\''))
                        {
                            c[clen - 1] = '\0';
                            memmove(c, c + 1, clen);
                        }
                        if (c[0]) final_title = str_dup(c);
                    }
                    free(no_think);
                }
            }
            free(raw);
        }
    }
    else if (resp)
    {
        llm_response_free(resp);
    }

    /* fall back to truncated first user message if LLM produced nothing */
    if (!final_title)
        final_title = str_dup(fallback);

    return final_title;
}

static int agent_apply_title(Agent *agent, const char *title)
{
    Session *s2 = session_manager_load_session_alloc(agent->sm, agent->session_id);
    if (!s2)
    {
        log_error("agent_apply_title: session reload failed",
                  "session_id", agent->session_id, NULL);
        return -1;
    }
    char *title_dup = str_dup(title);
    if (!title_dup)
    {
        log_error("agent_apply_title: OOM duplicating title", NULL);
        session_free(s2);
        return -1;
    }
    free(s2->title);
    s2->title = title_dup;
    if (session_manager_save_session(agent->sm, s2) != 0)
    {
        log_error("agent_apply_title: save failed",
                  "session_id", agent->session_id, NULL);
        session_free(s2);
        return -1;
    }
    session_free(s2);

    if (agent->on_title_update)
        agent->on_title_update(agent->session_id, title, agent->title_userdata);
    return 0;
}

int agent_generate_title(Agent *agent)
{
    if (!agent || !agent->provider) return -1;
    if (!agent->sm || !agent->session_id) return -1;
    if (agent->messages_count == 0) return -1;

    Session *s = session_manager_load_session_alloc(agent->sm, agent->session_id);
    if (!s || s->title_generation_attempted)
    {
        if (s) session_free(s);
        return -1;
    }
    session_free(s);

    s = session_manager_load_session_alloc(agent->sm, agent->session_id);
    if (!s)
    {
        log_error("agent_generate_title: session reload failed",
                  "session_id", agent->session_id, NULL);
        return -1;
    }
    s->title_generation_attempted = 1;
    if (session_manager_save_session(agent->sm, s) != 0)
    {
        log_error("agent_generate_title: save failed",
                  "session_id", agent->session_id, NULL);
        session_free(s);
        return -1;
    }
    session_free(s);

    /* find first user message — matching Python version's approach:
     * only the first user request, not the full conversation.
     * full-conversation excerpts confuse small models into producing
     * hallucinated placeholder titles like "(Waiting for ...)" */
    const char *first_user_msg = NULL;
    for (int i = 0; i < agent->messages_count; i++)
    {
        if (agent->messages[i].role && strcmp(agent->messages[i].role, "user") == 0
            && agent->messages[i].content && agent->messages[i].content[0])
        {
            first_user_msg = agent->messages[i].content;
            break;
        }
    }
    if (!first_user_msg) return -1;

    /* fallback: first 30 chars of user message, with "..." if truncated */
    char *fallback = NULL;
    size_t fblen = strlen(first_user_msg);
    if (fblen <= 30)
    {
        fallback = str_dup(first_user_msg);
    }
    else
    {
        if (asprintf(&fallback, "%.30s...", first_user_msg) < 0)
            fallback = NULL;
    }
    if (!fallback)
    {
        log_error("agent_generate_title: OOM building fallback title", NULL);
        return -1;
    }

    char *final_title = agent_title_from_model(agent->provider, agent->model,
                                               first_user_msg, fallback);
    int rc = 0;
    if (final_title)
    {
        rc = agent_apply_title(agent, final_title);
    }
    else
    {
        /* The model produced no title; the LLM failure is already logged
         * inside agent_title_from_model. */
        rc = -1;
    }

    free(final_title);
    free(fallback);
    return rc;
}
