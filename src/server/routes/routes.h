#ifndef ECHO_ROUTES_H
#define ECHO_ROUTES_H

#include <cjson/cJSON.h>
#include "../server.h"

typedef struct WSClient WSClient;

typedef struct {
    const char *method;
    const char *path;
    int is_prefix;
    int needs_unlock;
    int unlock_via_query;
    route_handler handler;
} Route;

extern const Route routes[];
extern const int routes_count;

int route_match(const char *method, const char *path, const Route *r);

void ws_add_message_to_json(cJSON *m, const Message *msg);
void server_sse_write(Client *client, const char *data);

#endif
