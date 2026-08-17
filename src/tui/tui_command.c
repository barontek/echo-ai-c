/*
 * tui_command.c - command registry implementation. Fixed-capacity table
 * of command metadata + handler pointers; slash-name resolution walks
 * primary names before aliases. Pure map logic, fully unit-testable.
 * Depends on: tui_command.h, stdlib, string.h.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "tui_command.h"

struct TuiCommandRegistry {
    TuiCommand commands[TUI_CMD_REGISTRY_MAX];
    int count;
};

TuiCommandRegistry *tui_command_registry_create(void)
{
    return calloc(1, sizeof(TuiCommandRegistry));
}

void tui_command_registry_destroy(TuiCommandRegistry *r)
{
    free(r);
}

/* Bounded copy that always NUL-terminates (fields are pre-zeroed). */
static void copy_field(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    snprintf(dst, cap, "%s", src);
}

int tui_command_register(TuiCommandRegistry *r, const TuiCommand *c)
{
    if (!r || !c) return -1;

    for (int i = 0; i < r->count; i++)
    {
        if (strcmp(r->commands[i].name, c->name) == 0)
        {
            /* keep registration order; replace everything but name */
            TuiCommand *dst = &r->commands[i];
            char name[TUI_CMD_NAME_MAX];
            memcpy(name, dst->name, sizeof(name));
            memset(dst, 0, sizeof(*dst));
            memcpy(dst->name, name, sizeof(name));
            copy_field(dst->title, sizeof(dst->title), c->title);
            copy_field(dst->desc, sizeof(dst->desc), c->desc);
            copy_field(dst->category, sizeof(dst->category), c->category);
            copy_field(dst->slash, sizeof(dst->slash), c->slash);
            copy_field(dst->aliases, sizeof(dst->aliases), c->aliases);
            dst->suggested = c->suggested;
            dst->hidden = c->hidden;
            dst->fn = c->fn;
            return 0;
        }
    }
    if (r->count >= TUI_CMD_REGISTRY_MAX) return -1;

    TuiCommand *dst = &r->commands[r->count];
    memset(dst, 0, sizeof(*dst));
    copy_field(dst->name, sizeof(dst->name), c->name);
    copy_field(dst->title, sizeof(dst->title), c->title);
    copy_field(dst->desc, sizeof(dst->desc), c->desc);
    copy_field(dst->category, sizeof(dst->category), c->category);
    copy_field(dst->slash, sizeof(dst->slash), c->slash);
    copy_field(dst->aliases, sizeof(dst->aliases), c->aliases);
    dst->suggested = c->suggested;
    dst->hidden = c->hidden;
    dst->fn = c->fn;
    r->count++;
    return 0;
}

const TuiCommand *tui_command_find(const TuiCommandRegistry *r, const char *name)
{
    if (!r || !name) return NULL;
    for (int i = 0; i < r->count; i++)
        if (strcmp(r->commands[i].name, name) == 0)
            return &r->commands[i];
    return NULL;
}

int tui_command_slash_matches(const TuiCommand *c, const char *slash)
{
    if (!c || !slash) return 0;
    if (c->slash[0] && strcmp(c->slash, slash) == 0) return 1;
    if (c->aliases[0])
    {
        /* comma-separated alias list */
        char buf[TUI_CMD_ALIASES_MAX];
        memcpy(buf, c->aliases, sizeof(buf));
        char *save = NULL;
        for (char *a = strtok_r(buf, ",", &save); a;
             a = strtok_r(NULL, ",", &save))
        {
            /* trim spaces */
            char *s = a;
            while (*s == ' ') s++;
            if (strcmp(s, slash) == 0) return 1;
        }
    }
    return 0;
}

const TuiCommand *tui_command_find_slash(const TuiCommandRegistry *r,
                                         const char *slash)
{
    if (!r || !slash) return NULL;
    for (int i = 0; i < r->count; i++)
        if (tui_command_slash_matches(&r->commands[i], slash))
            return &r->commands[i];
    return NULL;
}

int tui_command_count(const TuiCommandRegistry *r)
{
    return r ? r->count : 0;
}

const TuiCommand *tui_command_at(const TuiCommandRegistry *r, int idx)
{
    if (!r || idx < 0 || idx >= r->count) return NULL;
    return &r->commands[idx];
}

void tui_command_walk(const TuiCommandRegistry *r, TuiCommandWalkFn fn, void *ud)
{
    if (!r || !fn) return;
    for (int i = 0; i < r->count; i++)
        fn(ud, &r->commands[i]);
}