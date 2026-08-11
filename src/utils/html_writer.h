/*
 * html_writer.h - text writer and scoring contracts.
 * Depends on: html_internal.h, html_outbuf.h.
 */

#ifndef ECHO_HTML_WRITER_H
#define ECHO_HTML_WRITER_H

#include <stddef.h>

#include "html_internal.h"
#include "html_outbuf.h"

/**
 * writer_newline - request a paragraph boundary
 * @w: writer.
 *
 * The newline is emitted before the next text unless the output is
 * rolled back first.
 *
 * Return: void.
 */
void writer_newline(Writer *w);

/**
 * writer_preflush - emit a pending newline or space
 * @w: writer.
 *
 * Return: 0 on success, -1 on allocation failure.
 */
int writer_preflush(Writer *w);

/**
 * writer_text - append a text fragment with whitespace collapsing
 * @w: writer.
 * @s: bytes to append.
 * @n: byte count.
 *
 * Runs of whitespace collapse to one space; pending block newlines win
 * over pending spaces.
 *
 * Return: 0 on success, -1 on allocation failure.
 */
int writer_text(Writer *w, const char *s, size_t n);

/**
 * writer_sync_state - recompute last-char state after truncation
 * @w: writer.
 *
 * Return: void.
 */
void writer_sync_state(Writer *w);

/**
 * count_words - count whitespace-separated words
 * @s: bytes to scan.
 * @n: byte count.
 *
 * Return: the number of word runs in @s[0..n).
 */
size_t count_words(const char *s, size_t n);

/**
 * run_has_nonws - test whether a run contains a non-space byte
 * @s: bytes to scan.
 * @n: byte count.
 *
 * Return: 1 when any byte is not whitespace, 0 otherwise.
 */
int run_has_nonws(const char *s, size_t n);

/**
 * frame_score - Crawl4AI content score for a frame
 * @f: frame to score.
 *
 * Return: the weighted density/link/weight/class/length score.
 */
double frame_score(const Frame *f);

/**
 * append_title_text - render a title text run
 * @b: destination buffer.
 * @s: raw bytes (may contain entities and non-UTF-8 bytes).
 * @n: byte count.
 *
 * Decodes entities and transcodes non-UTF-8 bytes, collapsing
 * whitespace.
 *
 * Return: 0 on success, -1 on allocation failure.
 */
int append_title_text(OutBuf *b, const char *s, size_t n);

#endif /* ECHO_HTML_WRITER_H */
