/*
 * tui_autocomplete.h - slash-command completion: given an input starting
 * with '/', compute the best completion from the command registry's slash
 * names (primary + aliases). Pure model, fully unit-testable; the UI
 * calls it on Tab and replaces the input.
 * Depends on: tui_command.h.
 */

#ifndef ECHO_TUI_AUTOCOMPLETE_H
#define ECHO_TUI_AUTOCOMPLETE_H

#include <stddef.h>

#include "tui_command.h"

/**
 * tui_autocomplete_slash - complete a typed slash command
 * @r: command registry; non-NULL.
 * @input: the current input text; non-NULL. Must start with '/'.
 * @out: caller-owned buffer receiving the completed input; non-NULL.
 * @cap: capacity of @out; must be >= 1.
 *
 * The part after the leading '/' up to the first space is the prefix.
 * When exactly one command (primary name or alias) matches the prefix,
 * the whole input is replaced with "/<slash> " (trailing space, so the
 * user keeps typing). When several match, the input is completed to the
 * longest common prefix of their slash names. A full or empty input is a
 * no-op.
 *
 * Return: 1 when @out was written, 0 when nothing matched.
 */
int tui_autocomplete_slash(const TuiCommandRegistry *r, const char *input,
                           char *out, size_t cap);

#endif /* ECHO_TUI_AUTOCOMPLETE_H */