#ifndef ECHO_SESSION_H
#define ECHO_SESSION_H

#include <cjson/cJSON.h>
#include "../agent/message.h"

typedef struct {
    char *id;
    char *title;
    char *created_at;
    Message *messages;
    int messages_count;
    cJSON *metadata;
    cJSON *events;
} Session;

Session *session_create(const char *title);
void session_free(Session *session);
char *session_serialize_messages(const Session *session);
int session_deserialize_messages(Session *session, const char *json_str);
char *session_serialize_metadata(const Session *session);
int session_deserialize_metadata(Session *session, const char *json_str);

#endif
