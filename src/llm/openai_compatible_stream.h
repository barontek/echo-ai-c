/*
 * openai_compatible_stream.h - SSE stream parsing contracts for the
 * OpenAI-compatible client.
 * Depends on: openai_compatible.h, message.h.
 */

#ifndef ECHO_OPENAI_COMPATIBLE_STREAM_H
#define ECHO_OPENAI_COMPATIBLE_STREAM_H

#include <stddef.h>

#include "openai_compatible.h"

typedef struct {
    LLMResponse *resp;
    char *line;
    size_t line_len;
    size_t line_cap;
    int thinking_open; /* 1 while a <think> block is open in resp->content */
    void (*on_chunk)(const char *, void *);
    void *userdata;
} StreamParser;

/**
 * stream_parser_feed - deliver raw SSE bytes to the parser
 * @p: parser state (caller owns resp/on_chunk/userdata).
 * @bytes: bytes to consume.
 * @len: byte count.
 *
 * Buffers lines, strips CR, and dispatches "data: " events (skipping
 * "[DONE]") through parse_stream_event.
 *
 * Return: 0 on success, -1 on allocation or event failure. On -1 the
 * caller must free p->line.
 */
int stream_parser_feed(StreamParser *p, const char *bytes, size_t len);

/**
 * stream_parser_finish - flush a trailing partial line and close
 * @p: parser state.
 *
 * Processes a final unterminated "data: " line, then closes any still
 * open thinking block so the saved message parses.
 *
 * Return: 0 on success, -1 on allocation or event failure.
 */
int stream_parser_finish(StreamParser *p);

#endif /* ECHO_OPENAI_COMPATIBLE_STREAM_H */
