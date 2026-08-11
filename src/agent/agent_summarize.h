/*
 * agent_summarize.h - context-window summarization contracts.
 * Depends on: agent.h.
 */

#ifndef ECHO_AGENT_SUMMARIZE_H
#define ECHO_AGENT_SUMMARIZE_H

#include "agent.h"

/**
 * agent_perform_summarization - summarize the conversation into context
 * @agent: agent context.
 * @original_count: number of messages before windowing (unused; kept
 *   for call-site symmetry).
 *
 * Assembles the transcript and runs a summarization LLM call; the
 * result replaces agent->context_summary. Transcripts larger than 2x
 * the context budget are skipped.
 *
 * Return: 0 on success, -1 when the transcript is oversized, OOM, or
 * the model produced nothing usable.
 */
int agent_perform_summarization(Agent *agent, int original_count);

#endif /* ECHO_AGENT_SUMMARIZE_H */
