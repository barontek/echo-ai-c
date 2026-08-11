/*
 * openai_stream.h - Codex SSE stream parser contracts. The StreamParser
 * struct itself is in openai_internal.h (shared with the request
 * transport).
 * Depends on: openai_internal.h.
 */

#ifndef ECHO_OPENAI_STREAM_H
#define ECHO_OPENAI_STREAM_H

#include <stddef.h>

#include "openai_internal.h"

/**
 * stream_feed - deliver raw SSE bytes to the parser
 * @parser: parser state; the caller owns response/on_chunk/userdata.
 * @bytes: bytes to consume; may be NULL only when length is 0.
 * @length: byte count, bounded so the parser never exceeds
 *   OPENAI_MAX_RESPONSE_BYTES in total.
 *
 * Return: 0 on success, -1 on NULL parser, embedded NUL, line overflow,
 * or event parse failure. On -1 the parser state is left for cleanup.
 */
int stream_feed(StreamParser *parser, const void *bytes, size_t length);

/**
 * stream_finish - validate and close a stream
 * @parser: parser state.
 *
 * Flushes any buffered line/event, then requires a terminal event with
 * a completed response, no failure, and complete tool calls. Closes any
 * still-open thinking block.
 *
 * Return: 0 on success, -1 on validation failure.
 */
int stream_finish(StreamParser *parser);

/**
 * stream_parser_cleanup - free parser-owned allocations
 * @parser: parser to clean up; NULL is a no-op.
 *
 * Frees calls, line, and event_data; the response itself is NOT freed
 * (the caller owns it).
 *
 * Return: void.
 */
void stream_parser_cleanup(StreamParser *parser);

/**
 * reasoning_summary_join - join a reasoning item's summary texts
 * @item: cJSON reasoning item whose "summary" array holds
 *   {"type":"summary_text","text":"..."} entries.
 *
 * Return: caller-owned newline-joined string, or NULL when the item has
 * no readable summary or allocation fails.
 */
char *reasoning_summary_join(const cJSON *item);

#endif /* ECHO_OPENAI_STREAM_H */
