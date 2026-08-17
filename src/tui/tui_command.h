/*
 * tui_command.h - command registry: named commands with titles,
 * descriptions, categories, slash names and aliases, and a handler
 * function pointer. The keymap and the command palette both dispatch
 * through this registry, so a command's keys and its slash form are the
 * same object. Pure model (no terminal I/O); handlers receive an opaque
 * context pointer (the TuiApp in the TUI) plus optional slash arguments.
 * Depends on: stdlib.
 */

#ifndef ECHO_TUI_COMMAND_H
#define ECHO_TUI_COMMAND_H

#include <stddef.h>

#define TUI_CMD_NAME_MAX 48
#define TUI_CMD_TITLE_MAX 48
#define TUI_CMD_DESC_MAX 96
#define TUI_CMD_CATEGORY_MAX 24
#define TUI_CMD_SLASH_MAX 24
#define TUI_CMD_ALIASES_MAX 96
#define TUI_CMD_REGISTRY_MAX 128

/* Handler signature: ud is the caller's context (the TuiApp in the TUI),
 * args is the text after the slash command (NULL for keymap/palette
 * dispatches and commands that take no arguments). */
typedef void (*TuiCommandFn)(void *ud, const char *args);

typedef struct {
    char name[TUI_CMD_NAME_MAX];       /* canonical name, e.g. "session.list" */
    char title[TUI_CMD_TITLE_MAX];     /* palette title, e.g. "List sessions" */
    char desc[TUI_CMD_DESC_MAX];       /* palette description */
    char category[TUI_CMD_CATEGORY_MAX];
    char slash[TUI_CMD_SLASH_MAX];     /* primary slash name, no '/' */
    char aliases[TUI_CMD_ALIASES_MAX]; /* comma-separated extra slash names */
    int suggested;                     /* shown in the palette's Suggested head */
    int hidden;                        /* 1: not listed in the palette */
    TuiCommandFn fn;                   /* handler; NULL = metadata only */
} TuiCommand;

typedef struct TuiCommandRegistry TuiCommandRegistry;

/**
 * tui_command_registry_create - allocate an empty registry
 *
 * Return: caller-owned registry, or NULL on allocation failure. Release
 *   with tui_command_registry_destroy().
 */
TuiCommandRegistry *tui_command_registry_create(void);

/**
 * tui_command_registry_destroy - release a registry
 * @r: registry to release, or NULL (no-op).
 *
 * Return: void.
 */
void tui_command_registry_destroy(TuiCommandRegistry *r);

/**
 * tui_command_register - add (copy) a command to the registry
 * @r: registry; non-NULL.
 * @c: command to copy; non-NULL.
 *
 * Re-registering an existing name replaces the entry (last wins), so
 * plugins and later phases can override built-ins. Commands past the
 * registry cap are refused.
 *
 * Return: 0 on success, -1 on NULL input or a full registry.
 */
int tui_command_register(TuiCommandRegistry *r, const TuiCommand *c);

/**
 * tui_command_find - look up a command by canonical name
 * @r: registry; non-NULL.
 * @name: canonical name; non-NULL.
 *
 * Return: borrowed command, or NULL when unknown.
 */
const TuiCommand *tui_command_find(const TuiCommandRegistry *r, const char *name);

/**
 * tui_command_find_slash - look up a command by slash name
 * @r: registry; non-NULL.
 * @slash: slash name without the '/', e.g. "sessions"; non-NULL.
 *
 * Matches the primary slash name first, then the comma-separated
 * aliases. Matching is case-sensitive (slash commands are lowercase).
 *
 * Return: borrowed command, or NULL when no command owns that slash name.
 */
const TuiCommand *tui_command_find_slash(const TuiCommandRegistry *r,
                                         const char *slash);

/**
 * tui_command_count - number of registered commands
 * @r: registry; non-NULL.
 *
 * Return: the command count.
 */
int tui_command_count(const TuiCommandRegistry *r);

/**
 * tui_command_at - the command at an index
 * @r: registry; non-NULL.
 * @idx: 0-based index; must be < tui_command_count().
 *
 * Return: borrowed command, or NULL when @idx is out of range.
 */
const TuiCommand *tui_command_at(const TuiCommandRegistry *r, int idx);

/* Visitor for tui_command_walk. */
typedef void (*TuiCommandWalkFn)(void *ud, const TuiCommand *c);

/**
 * tui_command_walk - visit every command in registration order
 * @r: registry; non-NULL.
 * @fn: visitor; non-NULL.
 * @ud: passed through to fn.
 *
 * Return: void.
 */
void tui_command_walk(const TuiCommandRegistry *r, TuiCommandWalkFn fn, void *ud);

/**
 * tui_command_slash_matches - does a command own a slash name?
 * @c: command; non-NULL.
 * @slash: slash name without '/'; non-NULL.
 *
 * Return: 1 when @slash is the primary name or one of the aliases.
 */
int tui_command_slash_matches(const TuiCommand *c, const char *slash);

#endif /* ECHO_TUI_COMMAND_H */