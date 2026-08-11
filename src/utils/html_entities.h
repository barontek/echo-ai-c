/*
 * html_entities.h - HTML entity decoding contracts.
 * Depends on: html_unicode.h.
 */

#ifndef ECHO_HTML_ENTITIES_H
#define ECHO_HTML_ENTITIES_H

#include <stddef.h>

/**
 * decode_entity - decode the entity starting at s[0]
 * @s: input bytes; s[0] must be '&' to decode.
 * @avail: bytes available at s.
 * @out: receives 1-4 UTF-8 bytes.
 * @out_len: receives the byte count written.
 * @consumed: receives the input bytes advanced.
 *
 * Named-entity table values are CP1252 bytes (mapped through cp1252_cp);
 * numeric entities keep their full code point. Surrogates and out-of-
 * range values fail.
 *
 * Return: 1 on success, 0 when s[0] is not a recognized entity (the
 * caller emits '&' literally).
 */
int decode_entity(const char *s, size_t avail, char out[4],
                  size_t *out_len, size_t *consumed);

#endif /* ECHO_HTML_ENTITIES_H */
