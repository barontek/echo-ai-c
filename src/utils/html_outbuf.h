/*
 * html_outbuf.h - growable output buffer contracts.
 * Depends on: html_internal.h (OutBuf).
 */

#ifndef ECHO_HTML_OUTBUF_H
#define ECHO_HTML_OUTBUF_H

#include <stddef.h>

#include "html_internal.h"

/**
 * outbuf_reserve - ensure capacity for extra bytes plus NUL
 * @b: buffer to grow.
 * @extra: additional bytes needed.
 *
 * Return: 0 on success, -1 on overflow or allocation failure (buffer
 * unchanged).
 */
int outbuf_reserve(OutBuf *b, size_t extra);

/**
 * outbuf_append - append bytes and NUL-terminate
 * @b: buffer.
 * @s: bytes to append.
 * @n: byte count.
 *
 * Return: 0 on success, -1 on allocation failure (buffer unchanged).
 */
int outbuf_append(OutBuf *b, const char *s, size_t n);

/**
 * outbuf_append_chr - append one byte and NUL-terminate
 * @b: buffer.
 * @c: byte to append.
 *
 * Return: 0 on success, -1 on allocation failure.
 */
int outbuf_append_chr(OutBuf *b, char c);

/**
 * outbuf_truncate - roll the buffer back to a length
 * @b: buffer.
 * @len: new length; no-op when >= current length.
 *
 * Return: void.
 */
void outbuf_truncate(OutBuf *b, size_t len);

#endif /* ECHO_HTML_OUTBUF_H */
