/*
 * tui_autocomplete.c - slash-command completion. Collects every slash
 * name (primary + aliases) from the registry, filters by the typed
 * prefix, and completes to a single match or the longest common prefix.
 * Matched names are duplicated (never pointers into locals), so the
 * collection is ASan-clean.
 * Depends on: tui_autocomplete.h, stdlib, string.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tui_autocomplete.h"

/* Append a duplicated slash name to the match list; frees the whole list
 * and returns 0 on allocation failure. */
static int add_match(char **match, int *count, int cap, const char *name,
                     int *failed)
{
    if (*failed || *count >= cap) return 0;
    match[*count] = strdup(name);
    if (!match[*count])
    {
        *failed = 1;
        return 0;
    }
    (*count)++;
    return 1;
}

int tui_autocomplete_slash(const TuiCommandRegistry *r, const char *input,
                           char *out, size_t cap)
{
    if (!r || !input || !out || cap < 1) return 0;
    if (input[0] != '/') return 0;

    /* the prefix is the text after '/' up to the first space */
    const char *p = input + 1;
    size_t plen = 0;
    while (p[plen] && p[plen] != ' ') plen++;
    if (plen == 0) return 0;

    enum { MAX_MATCH = TUI_CMD_REGISTRY_MAX * 4 };
    char *match[MAX_MATCH];
    int match_count = 0;
    int failed = 0;
    int n = tui_command_count(r);
    for (int i = 0; i < n; i++)
    {
        const TuiCommand *c = tui_command_at(r, i);
        if (c->slash[0] && strncmp(c->slash, p, plen) == 0)
            add_match(match, &match_count, MAX_MATCH, c->slash, &failed);
        if (c->aliases[0])
        {
            char buf[TUI_CMD_ALIASES_MAX];
            memcpy(buf, c->aliases, sizeof(buf));
            char *save = NULL;
            for (char *a = strtok_r(buf, ",", &save); a;
                 a = strtok_r(NULL, ",", &save))
            {
                while (*a == ' ') a++;
                if (a[0] && strncmp(a, p, plen) == 0)
                    add_match(match, &match_count, MAX_MATCH, a, &failed);
            }
        }
    }
    if (failed)
    {
        for (int i = 0; i < match_count; i++) free(match[i]);
        return 0;
    }
    if (match_count == 0) return 0;

    int rc = 1;
    if (match_count == 1)
    {
        snprintf(out, cap, "/%s ", match[0]); // NOLINT(cert-err33-c)
    }
    else
    {
        /* longest common prefix across the matches */
        size_t lcp = strlen(match[0]);
        for (int i = 1; i < match_count; i++)
        {
            size_t j = 0;
            while (j < lcp && match[i][j] && match[i][j] == match[0][j]) j++;
            lcp = j;
        }
        if (lcp <= plen)
        {
            rc = 0; /* the common prefix does not extend the input */
        }
        else
        {
            char tmp[64];
            if (lcp >= sizeof(tmp)) lcp = sizeof(tmp) - 1;
            memcpy(tmp, match[0], lcp);
            tmp[lcp] = '\0';
            snprintf(out, cap, "/%s", tmp); // NOLINT(cert-err33-c)
        }
    }
    for (int i = 0; i < match_count; i++) free(match[i]);
    return rc;
}
