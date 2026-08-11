/*
 * html_unicode.h - Unicode/encoding helper contracts.
 * Depends on: stddef.
 */

#ifndef ECHO_HTML_UNICODE_H
#define ECHO_HTML_UNICODE_H

#include <stddef.h>

/**
 * utf8_encode_cp - encode a Unicode scalar as UTF-8
 * @cp: scalar value to encode.
 * @out: receives 1-4 UTF-8 bytes.
 *
 * Return: byte count written (1-4), or 0 for a non-scalar value
 * (surrogate, above U+10FFFF).
 */
size_t utf8_encode_cp(unsigned int cp, char out[4]);

/**
 * cp1252_cp - map a byte through the Windows-1252 single-byte table
 * @b: byte value.
 *
 * Return: the Unicode code point for @b (0x80-0x9F slots map to their
 * CP1252 characters; the latin-1 range passes through).
 */
unsigned int cp1252_cp(unsigned char b);

/**
 * utf8_valid - strict UTF-8 validation
 * @s: bytes to validate.
 * @n: byte count.
 *
 * Rejects overlong encodings, surrogates, values above U+10FFFF, and
 * truncated sequences.
 *
 * Return: 1 when @s[0..n) is valid UTF-8, 0 otherwise.
 */
int utf8_valid(const char *s, size_t n);

/**
 * utf8_cut_boundary - back a cut point off a UTF-8 continuation byte
 * @data: UTF-8 data.
 * @cut: proposed cut position.
 *
 * Return: the largest position <= @cut whose byte is not a continuation
 * byte, so cutting there never splits a multi-byte character.
 */
size_t utf8_cut_boundary(const char *data, size_t cut);

/**
 * prefix_ieq - case-insensitive prefix comparison
 * @s: string to test (must be long enough for the prefix).
 * @pat: NUL-terminated prefix.
 *
 * Return: 1 when the first strlen(pat) bytes of @s equal @pat ignoring
 * case, 0 otherwise.
 */
int prefix_ieq(const char *s, const char *pat);

#endif /* ECHO_HTML_UNICODE_H */
