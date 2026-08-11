/*
 * agent_internal.h - shared contracts for the agent subsystem, split
 * across agent (core), prompt, tools, title, summarize, and run units.
 * Documented exception to the one-header-per-module rule: the core's
 * append/save/time helpers and the AGENT_TEST realloc shim cross unit
 * boundaries, so their declarations live here instead of being
 * duplicated per file. The realloc #define that routes through the shim
 * stays per-TU after the shim bodies (matching the session module
 * pattern). Not installed or included outside src/agent.
 * Depends on: agent.h.
 */

#ifndef ECHO_AGENT_INTERNAL_H
#define ECHO_AGENT_INTERNAL_H

#include "agent.h"

/* agent.c (core) */
/**
 * time_sec - monotonic clock in seconds
 *
 * Return: seconds since an arbitrary fixed origin (CLOCK_MONOTONIC).
 */
double time_sec(void);

/**
 * agent_append_message - append a message struct to the agent's array
 * @agent: agent context.
 * @msg: message to append; the struct is copied in (fields keep their
 *   ownership), the caller's copy is NOT freed.
 *
 * Return: the new message index, or -1 on realloc failure (array
 * unchanged).
 */
int agent_append_message(Agent *agent, Message *msg);

/**
 * agent_save_session - persist the agent's messages to the session store
 * @agent: agent context.
 *
 * Loads (or mints) the session row, replaces its messages with the
 * agent's non-system messages, and saves while holding sm->lock.
 *
 * Return: void; failures are logged, never silent.
 */
void agent_save_session(Agent *agent);

#ifdef AGENT_TEST
/* Realloc shim defined non-static in agent.c under the same guard; the
 * counter lives there. TUs compiled with AGENT_TEST route realloc
 * through it via a per-TU #define after includes. */
void *agent_test_realloc(void *ptr, size_t size);
#endif

#endif /* ECHO_AGENT_INTERNAL_H */
