/*
 * agent_prompt.h - system prompt construction contracts.
 * Depends on: agent.h.
 */

#ifndef ECHO_AGENT_PROMPT_H
#define ECHO_AGENT_PROMPT_H

#include "agent.h"

/**
 * inject_system_with_summary - rebuild and inject the system message
 * @agent: agent context.
 *
 * Rebuilds the dynamic system prompt (cwd, time, persistent memory,
 * summary) and replaces the leading system message, inserting one when
 * absent.
 *
 * Return: 0 on success, -1 on prompt-build or insert OOM.
 */
int inject_system_with_summary(Agent *agent);

#endif /* ECHO_AGENT_PROMPT_H */
