/*
 * agent_tools.h - tool-call execution contracts.
 * Depends on: agent.h.
 */

#ifndef ECHO_AGENT_TOOLS_H
#define ECHO_AGENT_TOOLS_H

#include "agent.h"

/**
 * execute_tool_calls - run tool calls and record their result messages
 * @agent: agent context.
 * @calls: array of tool calls to execute.
 * @count: number of calls (>= 0).
 *
 * Executes each call through the tool registry (with safety approval
 * when configured), caps oversized results, and appends a tool message
 * per call. Failures are recorded as tool messages, never aborted.
 *
 * Return: 0 (always; per-call failures are recorded, not surfaced).
 */
int execute_tool_calls(Agent *agent, ToolCall *calls, int count);

#ifdef AGENT_TEST
/**
 * agent_test_execute_tool_calls - test seam for the tool executor
 * @agent: agent context.
 * @calls: array of tool calls.
 * @count: number of calls.
 *
 * Test-only hook forwarding to execute_tool_calls.
 *
 * Return: the executor's return value.
 */
int agent_test_execute_tool_calls(Agent *agent, ToolCall *calls, int count);
#endif

#endif /* ECHO_AGENT_TOOLS_H */
