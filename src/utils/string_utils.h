#ifndef ECHO_STRING_UTILS_H
#define ECHO_STRING_UTILS_H

char *str_trim(char *str);
char *str_dup(const char *str);
int str_starts_with(const char *str, const char *prefix);
int str_ends_with(const char *str, const char *suffix);

typedef struct {
    char **items;
    int count;
} StrArray;

StrArray str_split(const char *str, char delimiter);
void str_array_free(StrArray *arr);

char *sanitize_json(const char *str);

#endif
