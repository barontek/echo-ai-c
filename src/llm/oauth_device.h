/*
 * oauth_device.h - device-flow parsing helpers for the OpenAI OAuth
 * manager. The public device API (openai_oauth_device_start/poll) is
 * declared in openai_oauth.h.
 * Depends on: openai_oauth_internal.h.
 */

#ifndef ECHO_OAUTH_DEVICE_H
#define ECHO_OAUTH_DEVICE_H

#include "openai_oauth_internal.h"

/**
 * parse_device_authorization - parse a device token response
 * @response: JSON response body.
 * @code: receives a caller-owned authorization code (secure_free()).
 * @verifier: receives a caller-owned code verifier (secure_free()).
 *
 * Return: 0 on success, -1 on malformed input with both outputs NULL.
 */
int parse_device_authorization(const char *response, char **code,
                               char **verifier);

#endif /* ECHO_OAUTH_DEVICE_H */
