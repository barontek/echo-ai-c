/*
 * agent_prompt.c - system prompt construction: cwd, time,
 * persistent memory, conversation summary, and injection as
 * the leading system message.
 * Depends on: session/memory, unistd, time, logging.
 */

#define _GNU_SOURCE
#if defined(__linux__)
#define SYSTEM_OS "Linux"
#elif defined(__APPLE__)
#define SYSTEM_OS "macOS"
#elif defined(_WIN32)
#define SYSTEM_OS "Windows"
#else
#define SYSTEM_OS "Unknown"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "agent.h"
#include "agent_internal.h"
#include "agent_prompt.h"
#include "../session/session_manager.h"
#include "../session/memory.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

#ifdef AGENT_TEST
/* Fault-injection shim (counter lives in agent.c): route this
 * TU's realloc through the shared hook so
 * agent_test_set_realloc_fail() reaches the whole module. */
void *agent_test_realloc(void *ptr, size_t size);
#define realloc agent_test_realloc
#endif


static int build_system_prompt(Agent *agent, char **out, size_t *out_len)
{
    char cwd_buf[4096];
    const char *cwd = getcwd(cwd_buf, sizeof(cwd_buf));
    if (!cwd) cwd = ".";

    time_t now = time(NULL);
    /* D3: thread-safe localtime_r over thread-unsafe localtime. */
    struct tm tm_storage;
    struct tm *tm_info = localtime_r(&now, &tm_storage);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info); // NOLINT(cert-err33-c)

    char context_buf[512];
    snprintf(context_buf, sizeof(context_buf), // NOLINT(cert-err33-c)
             "\n\n[System Context]\nOS: " SYSTEM_OS "\n"
             "Current Working Directory: %s\nCurrent Time: %s\n",
             cwd, time_buf);

    char *mem_buf = NULL;
    if (agent->sm && agent->sm->db)
    {
        int mem_count = 0;
        MemoryFact *memories = memory_list_all(agent->sm->db, &mem_count, NULL);
        if (memories && mem_count > 0)
        {
            size_t mbsz = 512;
            mem_buf = malloc(mbsz);
            if (mem_buf)
            {
                size_t pos = 0;
                int w = snprintf(mem_buf, mbsz, "\n\n[Persistent Memory]\n");
                if (w > 0) pos = (size_t)w;
                int limit = mem_count < 64 ? mem_count : 64;
                for (int i = 0; i < limit; i++)
                {
                    size_t needed = pos + strlen(memories[i].key)
                                    + strlen(memories[i].value) + 12;
                    if (needed >= mbsz)
                    {
                        mbsz = needed + 256;
                        char *newbuf = realloc(mem_buf, mbsz);
                        if (!newbuf) {
                            free(mem_buf);
                            mem_buf = NULL;
                            break;
                        }
                        mem_buf = newbuf;
                    }
                    w = snprintf(mem_buf + pos, mbsz - pos,
                                 "%s = %s\n", memories[i].key, memories[i].value);
                    if (w > 0) pos += (size_t)w;
                }
            }
        }
        memory_facts_free(memories, mem_count);
    }

    const char *base = agent->system_prompt ? agent->system_prompt : "";
    if (agent->context_summary)
    {
        if (asprintf(out, "%s%s%s\n\nPrevious conversation summary: %s",
                     base, context_buf, mem_buf ? mem_buf : "",
                     agent->context_summary) < 0)
             {
                free(mem_buf);
                return -1;
            }
    }
    else
    {
        if (asprintf(out, "%s%s%s",
                     base, context_buf, mem_buf ? mem_buf : "") < 0)
             {
                free(mem_buf);
                return -1;
            }
    }
    free(mem_buf);
    if (out_len && *out) *out_len = strlen(*out);
    return 0;
}

int inject_system_with_summary(Agent *agent)
{
    char *sys = NULL;
    if (build_system_prompt(agent, &sys, NULL) != 0)
    {
        log_error("inject_system_with_summary: prompt build failed", NULL);
        return -1;
    }

    int found = 0;
    for (int i = 0; i < agent->messages_count; i++)
    {
        if (strcmp(agent->messages[i].role, "system") == 0)
        {
            free(agent->messages[i].content);
            agent->messages[i].content = sys;
            found = 1;
            break;
        }
    }

    if (!found)
    {
        Message sys_msg = {0};
        sys_msg.role = str_dup("system");
        sys_msg.content = sys;
        if (!sys_msg.role)
        {
            log_error("inject_system_with_summary: OOM duplicating role", NULL);
            free(sys);
            return -1;
        }
        Message *new_msgs = realloc(agent->messages, sizeof(Message) * (agent->messages_count + 1));
        if (!new_msgs)
        {
            log_error("inject_system_with_summary: OOM growing message array", NULL);
            free(sys_msg.role);
            free(sys);
            return -1;
        }
        memmove(new_msgs + 1, new_msgs, sizeof(Message) * agent->messages_count);
        new_msgs[0] = sys_msg;
        agent->messages = new_msgs;
        agent->messages_count++;
    }
    return 0;
}
