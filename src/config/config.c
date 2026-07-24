#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "config.h"
#include "../utils/string_utils.h"

#ifdef CONFIG_TEST
static int conf_alloc_counter = 0;
static int conf_alloc_fail_at = -1;

void config_test_set_alloc_fail(int nth_allocation)
{
    conf_alloc_counter = 0;
    conf_alloc_fail_at = nth_allocation;
}

static char *config_test_strdup(const char *s)
{
    conf_alloc_counter++;
    if (conf_alloc_counter == conf_alloc_fail_at) return NULL;
    return str_dup(s);
}

#define str_dup config_test_strdup
#endif

#define MAX_ENTRIES 256

typedef struct {
    char *key;
    char *value;
} Entry;

struct Conf {
    Entry entries[MAX_ENTRIES];
    int count;
};

static void trim_line(char *line)
{
    char *start = line;
    while (isspace((unsigned char)*start)) start++;

    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';

    if (start != line) memmove(line, start, end - start + 1);
}

Conf *conf_load(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    Conf *conf = calloc(1, sizeof(Conf));
    if (!conf) { fclose(fp); return NULL; }

    char section[256] = "";
    char line[4096];

    while (fgets(line, sizeof(line), fp))
    {
        trim_line(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        if (line[0] == '[')
        {
            char *end = strchr(line, ']');
            if (!end) continue;
            *end = '\0';
            size_t slen = strlen(line + 1);
            if (slen >= sizeof(section)) slen = sizeof(section) - 1;
            memcpy(section, line + 1, slen);
            section[slen] = '\0';
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *value = eq + 1;

        trim_line(key);
        trim_line(value);

        char *full_key = NULL;
        if (section[0])
        {
            if (asprintf(&full_key, "%s.%s", section, key) < 0)
            { free(full_key); continue; }
        }
        else
        {
            if (asprintf(&full_key, "%s", key) < 0)
            { free(full_key); continue; }
        }

        if (conf->count < MAX_ENTRIES)
        {
            char *k = str_dup(full_key);
            char *v = str_dup(value);
            if (k && v)
            {
                conf->entries[conf->count].key = k;
                conf->entries[conf->count].value = v;
                conf->count++;
            }
            else
            {
                free(k);
                free(v);
            }
        }
        free(full_key);
    }

    fclose(fp);
    return conf;
}

const char *conf_get(const Conf *conf, const char *key)
{
    for (int i = 0; i < conf->count; i++)
    {
        if (strcmp(conf->entries[i].key, key) == 0)
            return conf->entries[i].value;
    }
    return NULL;
}

int conf_get_int(const Conf *conf, const char *key, int def)
{
    const char *val = conf_get(conf, key);
    if (!val) return def;
    char *end = NULL;
    long result = strtol(val, &end, 10);
    if (end == val || *end != '\0') return def;
    return (int)result;
}

void conf_free(Conf *conf)
{
    if (!conf) return;
    for (int i = 0; i < conf->count; i++)
    {
        free(conf->entries[i].key);
        free(conf->entries[i].value);
    }
    free(conf);
}
