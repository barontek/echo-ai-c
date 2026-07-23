#ifndef ECHO_JSON_H
#define ECHO_JSON_H

#include <cjson/cJSON.h>

char *json_string_escape(const char *str);
cJSON *json_add_string(cJSON *obj, const char *key, const char *val);
cJSON *json_add_int(cJSON *obj, const char *key, int val);
cJSON *json_add_double(cJSON *obj, const char *key, double val);
char *json_serialize(cJSON *obj);

#endif
