/*
 * oauth_jwt.h - id_token parsing and JSON field helpers for the OpenAI
 * OAuth flows: JWT payload decoding, duplicate-key rejection, refresh
 * window arithmetic.
 * Depends on: cJSON, openai_oauth_internal.h.
 */

#ifndef ECHO_OAUTH_JWT_H
#define ECHO_OAUTH_JWT_H

#include <time.h>

#include <cjson/cJSON.h>

#include "openai_oauth_internal.h"

/**
 * json_string_field - extract a validated string field from a JSON object
 * @object: JSON object to read.
 * @name: field name to extract.
 * @required: 1 rejects a missing field, 0 treats it as success with
 *   *output untouched (still set to NULL).
 * @output: receives a caller-owned copy (free with string_dup's owner
 *   convention — free() or secure_free()).
 *
 * Return: 0 on success, -1 on NULL args, missing-required field, or a
 * non-string/oversized/control-character value.
 */
int json_string_field(cJSON *object, const char *name, int required,
                      char **output);

/**
 * exact_json_object - parse text that must be exactly one JSON object
 * @data: NUL-terminated JSON text, bounded by OAUTH_RESPONSE_MAX.
 * @output: receives a caller-owned cJSON object (cJSON_Delete()).
 *
 * Return: 0 on success, -1 on NULL args, parse failure, or trailing
 * non-whitespace content.
 */
int exact_json_object(const char *data, cJSON **output);

/**
 * json_keys_are_unique - reject objects with duplicate keys
 * @object: JSON object to inspect; non-objects fail.
 *
 * Return: 1 when every key in @object is unique, 0 otherwise.
 */
int json_keys_are_unique(cJSON *object);

/**
 * jwt_metadata - extract account id and plan type from an id_token
 * @jwt: NUL-terminated JWT (three dot-separated base64url segments).
 * @account: receives a caller-owned account id, or stays NULL if absent.
 * @plan: receives a caller-owned plan type, or stays NULL if absent.
 *
 * Return: 0 on success (both outputs may be NULL when the claims are
 * absent), -1 on malformed JWT or JSON, or failure to allocate either
 * output; on -1 both outputs are freed and NULL.
 */
int jwt_metadata(const char *jwt, char **account, char **plan);

/**
 * needs_refresh - decide whether a refresh is due
 * @expires_at: credential expiry timestamp.
 * @now: current time.
 *
 * Return: 1 when @expires_at is at or before @now, or within
 * OAUTH_REFRESH_SKEW_SECONDS of it; 0 otherwise.
 */
int needs_refresh(time_t expires_at, time_t now);

#endif /* ECHO_OAUTH_JWT_H */
