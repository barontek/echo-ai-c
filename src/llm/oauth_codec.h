/*
 * oauth_codec.h - URL encoding, PKCE, and base64url primitives for the
 * OpenAI OAuth flows. Pure helpers: no state, no I/O.
 * Depends on: OpenSSL (types), openai_oauth_internal.h for shared defines.
 */

#ifndef ECHO_OAUTH_CODEC_H
#define ECHO_OAUTH_CODEC_H

#include <stddef.h>

#include "openai_oauth_internal.h"

/**
 * string_dup - copy a string with overflow guard
 * @value: NUL-terminated string to copy; NULL is accepted.
 *
 * Return: caller-owned malloc'd copy, or NULL on NULL input or
 * allocation failure. Free with free().
 */
char *string_dup(const char *value);

/**
 * url_encode - percent-encode a string for query/form use
 * @value: NUL-terminated string; NULL is accepted.
 *
 * Return: caller-owned encoded string (free with free()), or NULL on
 * NULL input or overflow/allocation failure.
 */
char *url_encode(const char *value);

/**
 * pkce_challenge - derive the S256 PKCE challenge for a verifier
 * @verifier: NUL-terminated code verifier; NULL is accepted.
 *
 * Return: caller-owned base64url challenge (free with free()), or NULL
 * on NULL input or failure.
 */
char *pkce_challenge(const char *verifier);

/**
 * random_string - generate a 32-byte base64url random string
 * @output: receives a caller-owned string (free with free()).
 *
 * Return: 0 on success, -1 on NULL output or RNG failure.
 */
int random_string(char **output);

/**
 * make_pkce - generate a verifier/challenge pair
 * @verifier: receives the caller-owned verifier (secure_free()).
 * @challenge: receives the caller-owned challenge (free()).
 *
 * Return: 0 on success, -1 on failure with both outputs NULL.
 */
int make_pkce(char **verifier, char **challenge);

/**
 * build_authorize_url_values - build the issuer authorize URL query
 * @state: OAuth state value.
 * @challenge: PKCE challenge.
 *
 * Return: caller-owned full authorize URL (free with free()), or NULL
 * on allocation failure. NULL inputs are accepted and encoded empty.
 */
char *build_authorize_url_values(const char *state, const char *challenge);

/**
 * url_decode_exact - percent-decode an exact-length buffer
 * @value: raw bytes to decode.
 * @len: byte length of @value; 0 or SIZE_MAX are rejected.
 *
 * Return: caller-owned decoded string (free with free()), or NULL on
 * invalid input or allocation failure.
 */
char *url_decode_exact(const unsigned char *value, size_t len);

/**
 * base64url_decode - decode a base64url payload
 * @input: NUL-terminated base64url string.
 * @output_len: receives the decoded length; must be non-NULL.
 *
 * Return: caller-owned decoded bytes (free with free()), or NULL on
 * invalid input or allocation failure. Rejects lengths of 1 mod 4.
 */
unsigned char *base64url_decode(const char *input, size_t *output_len);

#endif /* ECHO_OAUTH_CODEC_H */
