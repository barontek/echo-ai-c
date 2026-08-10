/*
 * session.h - in-memory Session lifecycle: create/free, and JSON
 * serialization/deserialization of messages, metadata, and events.
 * Depends on: cJSON, agent/message.h.
 */

#ifndef ECHO_SESSION_H
#define ECHO_SESSION_H

#include <cjson/cJSON.h>
#include "../agent/message.h"

/* A session owns every field: id/title/created_at are malloc'd strings, the
 * messages array is message_clear()'d element-wise, and metadata/events are
 * owned cJSON trees. After session_create() and after every successful
 * deserialize, metadata/events are non-NULL. Freed by session_free(). */
typedef struct {
    char *id;
    char *title;
    int title_generation_attempted;
    char *created_at;
    Message *messages;
    int messages_count;
    cJSON *metadata;
    cJSON *events;
} Session;

/**
 * session_create - mint a new in-memory session
 * @title: title string, borrowed for the call duration; NULL/empty yields
 *   the default "New Session".
 *
 * Mints an id ("<unix-ts>-<rand>" suffix guards same-second collisions),
 * a "%Y-%m-%dT%H:%M:%S" created_at, an empty metadata object, and an empty
 * events array. No I/O — persistence is the caller's job.
 *
 * Return: caller-owned Session, or NULL on any allocation failure (all
 * partial state is freed before returning). Free with session_free().
 * Thread-safe; no shared state (uses localtime_r, not localtime).
 */
Session *session_create(const char *title);

/**
 * session_free - release a session and everything it owns
 * @session: session to free, or NULL (no-op).
 *
 * Frees id/title/created_at, message_clear()s every message in the array,
 * cJSON_Deletes metadata and events, then frees the struct. After the call
 * @session is dangling.
 *
 * Return: void.
 */
void session_free(Session *session);

/**
 * session_serialize_messages_new - serialize the message array to JSON
 * @session: session whose messages are read; must be non-NULL.
 *
 * The result is a NUL-terminated JSON array string, at minimum "[]" for an
 * empty (or never-populated) array.
 *
 * Return: caller-owned malloc'd string, or NULL on cJSON-print OOM. Free
 * with free(). Thread-safe; no shared state.
 */
char *session_serialize_messages_new(const Session *session);

/**
 * session_deserialize_messages - replace the message array from JSON
 * @session: session to mutate; must be non-NULL.
 * @json_str: JSON array string; must be non-NULL. Borrowed for the call
 *   duration.
 *
 * Frees the prior array (if any) and parses @json_str into it. On parse
 * failure or non-array input the prior messages are left unchanged. On a
 * mid-parse allocation failure the array is freed and reset to NULL/0.
 *
 * Return: 0 on success, -1 on parse or allocation failure. Thread-safe; no
 * shared state.
 */
int session_deserialize_messages(Session *session, const char *json_str);

/**
 * session_serialize_metadata_new - serialize the metadata object to JSON
 * @session: session whose metadata is read; must be non-NULL.
 *
 * Returns NULL iff session->metadata is NULL ("never set"); an empty
 * object normalizes to the canonical "{}".
 *
 * Return: caller-owned malloc'd string, or NULL on OOM (cJSON print or the
 * "{}" normalization). Free with free(). Thread-safe; no shared state.
 */
char *session_serialize_metadata_new(const Session *session);

/**
 * session_deserialize_metadata - replace the metadata object from JSON
 * @session: session to mutate; must be non-NULL.
 * @json_str: JSON object string; must be non-NULL. Borrowed for the call
 *   duration.
 *
 * Deletes the prior metadata tree unconditionally, then parses @json_str.
 * On parse failure the metadata is reset to a fresh empty object so the
 * non-NULL invariant holds; on a NULL @json_str the prior metadata is left
 * untouched.
 *
 * Return: 0 on success, -1 on NULL input or parse failure. Thread-safe; no
 * shared state.
 */
int session_deserialize_metadata(Session *session, const char *json_str);

/**
 * session_serialize_events_new - serialize the events array to JSON
 * @session: session whose events are read; must be non-NULL.
 *
 * Returns NULL iff session->events is NULL ("no events"); an empty array
 * normalizes to the canonical "[]".
 *
 * Return: caller-owned malloc'd string, or NULL on OOM (cJSON print or the
 * "[]" normalization). Free with free(). Thread-safe; no shared state.
 */
char *session_serialize_events_new(const Session *session);

/**
 * session_deserialize_events - replace the events array from JSON
 * @session: session to mutate; must be non-NULL.
 * @json_str: JSON array string; must be non-NULL. Borrowed for the call
 *   duration.
 *
 * Deletes the prior events tree unconditionally, then parses @json_str.
 * On parse failure or non-array input the events are reset to a fresh
 * empty array so the non-NULL invariant holds; on a NULL @json_str the
 * prior events are left untouched.
 *
 * Return: 0 on success, -1 on NULL input or parse/type failure. Thread-safe;
 * no shared state.
 */
int session_deserialize_events(Session *session, const char *json_str);

#endif
