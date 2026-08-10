/*
 * context.h - context-window management: message trimming, prioritization,
 * and thinking-content splitting for the agent's message list.
 * Depends on: message.h.
 */

#ifndef ECHO_CONTEXT_H
#define ECHO_CONTEXT_H

#include "message.h"

/**
 * split_thinking_content_dup - truncate a message at the last closed <think> block
 * @raw: raw message text, borrowed for the duration of the call. NULL is
 *   accepted and returns NULL.
 *
 * Returns a copy of everything through the end of the last *closed*
 * </think> tag, dropping any content after it (including a trailing
 * unterminated <think>); when no block ever closes, an unchanged copy is
 * returned.
 *
 * Return: caller-owned malloc'd string (free with free()), or NULL on
 * allocation failure. Thread-safe; no shared state.
 */
char *split_thinking_content_dup(const char *raw);

/**
 * apply_context_window - trim a message list to message/char budgets
 * @msgs: message array to trim, borrowed. The array itself is never freed
 *   here — the caller keeps ownership either way.
 * @count: in/out; on entry the number of messages, on return the number
 *   kept. Must be non-NULL.
 * @max_messages: maximum number of messages to keep.
 * @max_chars: maximum total content length (chars); converted internally
 *   to a token budget at 4 chars/token.
 *
 * Returns msgs unchanged (and *count unchanged) when the list already fits
 * both budgets; otherwise deep-copies and trims via smart_select_alloc() and
 * trim_messages_by_tokens_new(). When a new array is returned the caller must
 * free it with message_free_all() and free the original msgs itself.
 *
 * Return: original msgs when already within budget or on any allocation
 * failure (never NULL); otherwise a new caller-owned array with *count set
 * to the kept count. Thread-safe; no shared state.
 */
Message *apply_context_window(Message *msgs, int *count,
                              int max_messages, int max_chars);

/**
 * smart_select_alloc - deep-copy a priority-selected subset of messages
 * @msgs: source messages, borrowed; untouched and still owned by the
 *   caller.
 * @count: number of messages in msgs.
 * @keep_count: maximum number of messages to keep. keep_count <= 0
 *   selects nothing and returns NULL for a non-empty list (an empty list
 *   yields an empty copy).
 *
 * Selection order: system messages, tool messages (paired with their
 * preceding assistant message), the most recent 40% of the budget, then an
 * evenly sampled remainder; pass 2 un-orphans kept tool/assistant pairs.
 *
 * Return: caller-owned deep copy of the selected messages (free with
 * message_free_all()), or NULL on allocation failure or when nothing is
 * selected. Thread-safe; no shared state.
 */
Message *smart_select_alloc(Message *msgs, int count, int keep_count);

/**
 * trim_messages_by_tokens_new - drop oldest messages to fit a token budget
 * @msgs: message array to trim. On a successful trim this array is freed
 *   here (via message_free_all()) — ownership transfers to the returned
 *   array. When no trim happens the caller keeps ownership of msgs.
 * @count: in/out; on entry the number of messages, on return the number
 *   kept. Must be non-NULL.
 * @max_tokens: token budget; each message is estimated at
 *   strlen(content)/4 + 1 tokens.
 *
 * Keeps all system messages, the most recent messages that fit the budget,
 * and un-orphans tool/assistant pairs.
 *
 * Return: msgs unchanged (caller keeps ownership, *count unchanged) when
 * the list already fits, when allocation fails, or when *count is 0;
 * otherwise a new caller-owned array with *count set. Thread-safe; no
 * shared state.
 */
Message *trim_messages_by_tokens_new(Message *msgs, int *count, int max_tokens);

#ifdef CONTEXT_TEST
/**
 * context_test_set_alloc_fail - make the Nth allocation fail here
 * @nth_allocation: 1-based index of the next calloc/str_dup/realloc call
 *   to fail; -1 disables fault injection.
 *
 * Test-only hook. Resets the shared call counter, fails the Nth
 * allocation (only that call), and leaves every other allocation to
 * behave normally. Single-threaded tests only.
 *
 * Return: nothing.
 */
void context_test_set_alloc_fail(int nth_allocation);
#endif

#endif
