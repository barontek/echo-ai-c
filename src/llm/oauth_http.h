/*
 * oauth_http.h - curl plumbing for the OpenAI OAuth token and device
 * endpoints: request body building, cancellable transfers, and response
 * classification.
 * Depends on: openai_oauth_internal.h, oauth_codec.h, oauth_vault.h.
 */

#ifndef ECHO_OAUTH_HTTP_H
#define ECHO_OAUTH_HTTP_H

#include "openai_oauth_internal.h"

/**
 * exchange_token - POST a grant to the token endpoint
 * @auth: manager used for cancellation checks (lock is taken internally).
 * @generation: the login generation that must still be current.
 * @grant_type: "authorization_code" or "refresh_token".
 * @value: the code or refresh token (cleansed internally).
 * @redirect_uri: redirect URI for the authorization_code grant; may be
 *   NULL for refresh_token.
 * @verifier: PKCE verifier for the authorization_code grant; may be NULL
 *   for refresh_token.
 * @json_out: receives a caller-owned response body on OK; the caller
 *   must cleanse and free it.
 *
 * Return: OPENAI_OAUTH_TOKEN_OK, TRANSIENT, PERMANENT, or CANCELLED.
 * On non-OK, *json_out is NULL.
 */
OpenAIOAuthTokenResult exchange_token(OpenAIOAuth *auth, uint64_t generation,
    const char *grant_type, const char *value, const char *redirect_uri,
    const char *verifier, char **json_out);

/**
 * device_post - POST a JSON body to a device-flow endpoint
 * @auth: manager used for cancellation checks.
 * @generation: the login generation that must still be current.
 * @path: issuer-relative path.
 * @body: JSON request body (borrowed).
 * @status_out: receives the HTTP status code.
 * @response_out: receives a caller-owned response body on OK; the caller
 *   must cleanse and free it.
 *
 * Return: OPENAI_OAUTH_TOKEN_OK, TRANSIENT, or CANCELLED.
 */
OpenAIOAuthTokenResult device_post(OpenAIOAuth *auth, uint64_t generation,
    const char *path, const char *body, long *status_out, char **response_out);

/**
 * device_json_body - build the device-flow request body
 * @device_auth_id: device auth id; NULL omits the field.
 * @user_code: user code; NULL omits the field.
 *
 * Return: caller-owned JSON string (free with free()), or NULL on
 * allocation failure.
 */
char *device_json_body(const char *device_auth_id, const char *user_code);

/**
 * parse_device_start - parse a device-start response
 * @response: JSON response body.
 * @device_auth_id: receives a caller-owned device auth id.
 * @user_code: receives a caller-owned user code.
 * @interval: receives the poll interval in seconds (1..300).
 * @expires_in: receives the expiry in seconds (1..3600).
 *
 * Return: 0 on success, -1 on malformed input; on failure the outputs
 * are NULL.
 */
int parse_device_start(const char *response, char **device_auth_id,
                       char **user_code, unsigned int *interval,
                       unsigned int *expires_in);

#endif /* ECHO_OAUTH_HTTP_H */
