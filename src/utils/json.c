/*
 * json.c - thin helpers over cJSON: string escaping, keyed adds with
 * NULL-tolerant values, and unformatted serialization.
 * Depends on: cJSON, stdlib.
 */

#include <stdlib.h>
#include <string.h>

#include "json.h"

char *json_string_escape_dup(const char *str)
{
    if (!str) return NULL;
    size_t len = strlen(str);
    size_t cap = len * 2 + 1;
    char *esc = malloc(cap);
    if (!esc) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (j + 2 > cap) { free(esc); return NULL; }
        switch (str[i])
        {
        case '"':  esc[j++] = '\\'; esc[j++] = '"';  break;
        case '\\': esc[j++] = '\\'; esc[j++] = '\\'; break;
        case '\n': esc[j++] = '\\'; esc[j++] = 'n';  break;
        case '\t': esc[j++] = '\\'; esc[j++] = 't';  break;
        case '\r': esc[j++] = '\\'; esc[j++] = 'r';  break;
        default:   esc[j++] = str[i];
        }
    }
    esc[j] = '\0';
    return esc;
}

cJSON *json_add_string(cJSON *obj, const char *key, const char *val)
{
    return cJSON_AddStringToObject(obj, key, val ? val : "");
}

cJSON *json_add_int(cJSON *obj, const char *key, int val)
{
    return cJSON_AddNumberToObject(obj, key, val);
}

cJSON *json_add_double(cJSON *obj, const char *key, double val)
{
    return cJSON_AddNumberToObject(obj, key, val);
}

char *json_serialize(cJSON *obj)
{
    return cJSON_PrintUnformatted(obj);
}
