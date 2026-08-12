/*
 * config.c - INI-style config parser: [section] keys, continuation
 * lines, and [providers] token lookup.
 * Depends on: string_utils, stdio file I/O.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

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
    if (!conf) {
        fclose(fp);
        return NULL;
    }

    char section[256] = "";
    char line[4096];
    int last_entry_idx = -1;  /* track last entry for multi-line continuation */

    while (fgets(line, sizeof(line), fp))
    {
        /* remove trailing newline if present */
        size_t llen = strlen(line);
        if (llen > 0 && line[llen - 1] == '\n') line[llen - 1] = '\0';

        char *raw = line;
        while (isspace((unsigned char)*raw)) raw++;

        if (raw[0] == '\0' || raw[0] == '#') continue;

        if (raw[0] == '[')
        {
            char *end = strchr(raw, ']');
            if (!end) continue;
            *end = '\0';
            size_t slen = strlen(raw + 1);
            if (slen >= sizeof(section)) slen = sizeof(section) - 1;
            memcpy(section, raw + 1, slen);
            section[slen] = '\0';
            last_entry_idx = -1;
            continue;
        }

        char *eq = strchr(raw, '=');
        if (eq)
        {
            *eq = '\0';
            char *key = raw;
            char *value = eq + 1;

            trim_line(key);
            trim_line(value);

            char *full_key = NULL;
            if (section[0])
            {
                if (asprintf(&full_key, "%s.%s", section, key) < 0)
                 {
                    free(full_key);
                    continue;
                }
            }
            else
            {
                if (asprintf(&full_key, "%s", key) < 0)
                 {
                    free(full_key);
                    continue;
                }
            }

            /* fixed-size table: entries past MAX_ENTRIES are silently dropped */
            if (conf->count < MAX_ENTRIES)
            {
                char *k = str_dup(full_key);
                char *v = str_dup(value);
                if (k && v)
                {
                    conf->entries[conf->count].key = k;
                    conf->entries[conf->count].value = v;
                    last_entry_idx = conf->count;
                    conf->count++;
                }
                else
                {
                    free(k);
                    free(v);
                    last_entry_idx = -1;
                }
            }
            free(full_key);
        }
        else if (last_entry_idx >= 0)
        {
            /* continuation line: append to the previous entry's value */
            char *cur = conf->entries[last_entry_idx].value;
            size_t cur_len = strlen(cur);
            size_t add_len = strlen(raw);
            char *new_val = realloc(cur, cur_len + 1 + add_len + 1);
            if (new_val)
            {
                new_val[cur_len] = '\n';
                memcpy(new_val + cur_len + 1, raw, add_len + 1);
                conf->entries[last_entry_idx].value = new_val;
            }
        }
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
    /* C13: clamp to the int range — a config value of 2^40 would
     * otherwise become an implementation-defined (and negative) int that
     * later feeds overflow-prone arithmetic. */
    if (result > INT_MAX) return def;
    if (result < INT_MIN) return def;
    return (int)result;
}

#define PROVIDERS_PREFIX "providers."

int conf_provider_tokens_alloc(const Conf *conf, ConfToken **out, int *out_count)
{
    if (!conf || !out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;

    /* Count [providers] entries to size the array in one pass. */
    int n = 0;
    for (int i = 0; i < conf->count; i++)
    {
        if (strncmp(conf->entries[i].key, PROVIDERS_PREFIX,
                    sizeof(PROVIDERS_PREFIX) - 1) == 0) n++;
    }
    if (n == 0) return 0;

    ConfToken *tokens = calloc((size_t)n, sizeof(ConfToken));
    if (!tokens) return -1;

    int idx = 0;
    for (int i = 0; i < conf->count && idx < n; i++)
    {
        const char *key = conf->entries[i].key;
        if (strncmp(key, PROVIDERS_PREFIX, sizeof(PROVIDERS_PREFIX) - 1) != 0)
            continue;
        const char *name = key + sizeof(PROVIDERS_PREFIX) - 1;
        if (name[0] == '\0') continue;  /* bare "providers." key, no provider name */

        char *provider = str_dup(name);
        char *token = str_dup(conf->entries[i].value);
        if (!provider || !token)
        {
            free(provider);
            free(token);
            conf_token_list_free(tokens, idx);
            return -1;
        }
        tokens[idx].provider = provider;
        tokens[idx].token = token;
        idx++;
    }

    *out = tokens;
    *out_count = idx;
    return 0;
}

void conf_token_list_free(ConfToken *tokens, int count)
{
    if (!tokens) return;
    for (int i = 0; i < count; i++)
    {
        free(tokens[i].provider);
        free(tokens[i].token);
    }
    free(tokens);
}

const char *conf_provider_token(const Conf *conf, const char *provider)
{
    if (!conf || !provider) return NULL;

    /* OpenCode Zen and OpenCode Go share one token, so the key in
     * [providers] is "opencode". */
    const char *key_name = (strcmp(provider, "opencode_zen") == 0 ||
                            strcmp(provider, "opencode_go") == 0)
                               ? "opencode" : provider;

    char key[96];
    int len = snprintf(key, sizeof(key), "%s%s", PROVIDERS_PREFIX, key_name);
    if (len < 0 || (size_t)len >= sizeof(key)) return NULL;
    return conf_get(conf, key);
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
