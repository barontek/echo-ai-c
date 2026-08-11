/*
 * agent_title.h - title generation contracts.
 * Depends on: agent.h.
 */

#ifndef ECHO_AGENT_TITLE_H
#define ECHO_AGENT_TITLE_H

#include "agent.h"

/**
 * agent_generate_title - generate and persist a session title
 * @agent: agent context.
 *
 * Runs the title LLM call on the first user message, strips think tags
 * and quotes, falls back to a truncated user message, and persists the
 * result on the session (once per session).
 *
 * Return: 0 on success, -1 when the session is not savable, the model
 * produced nothing usable, or persistence failed.
 */
int agent_generate_title(Agent *agent);

#endif /* ECHO_AGENT_TITLE_H */
