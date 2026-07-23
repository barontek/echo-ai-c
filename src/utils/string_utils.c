#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "string_utils.h"

char *str_trim(char *str)
{
    if (!str) return NULL;

    char *start = str;
    while (isspace((unsigned char)*start)) start++;

    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';

    if (start != str) memmove(str, start, end - start + 1);
    return str;
}

char *str_dup(const char *str)
{
    if (!str) return NULL;
    size_t len = strlen(str);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, str, len + 1);
    return copy;
}

int str_starts_with(const char *str, const char *prefix)
{
    if (!str || !prefix) return 0;
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

int str_ends_with(const char *str, const char *suffix)
{
    if (!str || !suffix) return 0;
    size_t slen = strlen(str);
    size_t suflen = strlen(suffix);
    if (suflen > slen) return 0;
    return strcmp(str + slen - suflen, suffix) == 0;
}

StrArray str_split(const char *str, char delimiter)
{
    StrArray result = {NULL, 0};
    if (!str) return result;

    int capacity = 8;
    result.items = malloc(sizeof(char *) * capacity);
    if (!result.items) return result;

    const char *start = str;
    const char *p = str;

    while (*p)
    {
        if (*p == delimiter)
        {
            if (result.count >= capacity)
            {
                capacity *= 2;
                char **new_items = realloc(result.items, sizeof(char *) * capacity);
                if (!new_items) { str_array_free(&result); return result; }
                result.items = new_items;
            }

            size_t len = p - start;
            result.items[result.count] = malloc(len + 1);
            if (result.items[result.count])
            {
                memcpy(result.items[result.count], start, len);
                result.items[result.count][len] = '\0';
                result.count++;
            }
            start = p + 1;
        }
        p++;
    }

    if (result.count >= capacity)
    {
        capacity *= 2;
        char **new_items = realloc(result.items, sizeof(char *) * capacity);
        if (!new_items) { str_array_free(&result); return result; }
        result.items = new_items;
    }

    result.items[result.count] = str_dup(start);
    if (result.items[result.count]) result.count++;

    return result;
}

void str_array_free(StrArray *arr)
{
    if (!arr || !arr->items) return;
    for (int i = 0; i < arr->count; i++) free(arr->items[i]);
    free(arr->items);
    arr->items = NULL;
    arr->count = 0;
}
