/*
 * json.h - thin helpers over cJSON for escaping, keyed adds, and
 * serialization. Depends on: cJSON.
 */

#ifndef ECHO_JSON_H
#define ECHO_JSON_H

#include <cjson/cJSON.h>

/**
 * json_string_escape_dup - escape a C string for embedding in a JSON string
 * @str: NUL-terminated input; NULL yields NULL.
 *
 * Escapes '"', '\\', newline, tab, and carriage return; other bytes pass
 * through unchanged (no UTF-8 handling).
 *
 * Return: freshly malloc'd NUL-terminated string owned by the caller
 * (free with free()), or NULL on allocation failure. Pure function; no
 * shared state; safe to call concurrently.
 */
char *json_string_escape_dup(const char *str);

/**
 * json_add_string - add a string field to a JSON object
 * @obj: cJSON object to extend; must outlive the returned item.
 * @key: field name, NUL-terminated.
 * @val: field value; NULL is coerced to "".
 *
 * The returned item is owned by obj's tree — do not free it separately;
 * it dies with cJSON_Delete(obj).
 *
 * Return: pointer to the added cJSON item, or NULL on allocation
 * failure. Thread-safety: no shared state in the wrapper, but obj must
 * not be concurrently modified; the caller serializes access.
 */
cJSON *json_add_string(cJSON *obj, const char *key, const char *val);

/**
 * json_add_int - add an integer field to a JSON object
 * @obj: cJSON object to extend; must outlive the returned item.
 * @key: field name, NUL-terminated.
 * @val: integer value.
 *
 * The returned item is owned by obj's tree — do not free it separately;
 * it dies with cJSON_Delete(obj).
 *
 * Return: pointer to the added cJSON item, or NULL on allocation
 * failure. Thread-safety: no shared state in the wrapper, but obj must
 * not be concurrently modified; the caller serializes access.
 */
cJSON *json_add_int(cJSON *obj, const char *key, int val);

/**
 * json_add_double - add a floating-point field to a JSON object
 * @obj: cJSON object to extend; must outlive the returned item.
 * @key: field name, NUL-terminated.
 * @val: floating-point value.
 *
 * The returned item is owned by obj's tree — do not free it separately;
 * it dies with cJSON_Delete(obj).
 *
 * Return: pointer to the added cJSON item, or NULL on allocation
 * failure. Thread-safety: no shared state in the wrapper, but obj must
 * not be concurrently modified; the caller serializes access.
 */
cJSON *json_add_double(cJSON *obj, const char *key, double val);

/**
 * json_serialize - render a cJSON tree as a compact JSON string
 * @obj: tree to render; not modified. NULL is accepted by cJSON and
 *   yields a NULL return.
 *
 * Return: freshly malloc'd NUL-terminated JSON string owned by the
 * caller (free with free()), or NULL on allocation failure. Thread-
 * safety: no shared state, but obj must not be concurrently modified.
 */
char *json_serialize(cJSON *obj);

#endif
