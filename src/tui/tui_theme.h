/*
 * tui_theme.h - single source of visual truth: palettes, role->color/style
 * mapping, density, and spinner frames. Pane modules must call
 * tui_theme_color()/tui_theme_styles() — no hardcoded colors outside this
 * module. Pure logic (no notcurses types) so it is fully unit-testable.
 * Depends on: stdint.
 */

#ifndef ECHO_TUI_THEME_H
#define ECHO_TUI_THEME_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    TUI_ROLE_BASE_FG,
    TUI_ROLE_BASE_BG,
    TUI_ROLE_ACCENT,
    TUI_ROLE_USER,       /* user message marker + border of user blocks */
    TUI_ROLE_ASSISTANT,  /* assistant message body */
    TUI_ROLE_THINK,      /* <think> blocks (muted color, dimmed, italic) */
    TUI_ROLE_TOOL,       /* tool activity lines (dimmed) */
    TUI_ROLE_TOOL_RESULT, /* tool result blocks (distinct color) */
    TUI_ROLE_ERROR,      /* error lines (reverse video) */
    TUI_ROLE_STATUS_FG,  /* status bar text */
    TUI_ROLE_STATUS_BG,  /* status bar background */
    TUI_ROLE_BORDER,     /* pane borders */
    TUI_ROLE_MODAL_BORDER, /* focused modal border */
    TUI_ROLE_BACKDROP,   /* modal backdrop (obscures content behind) */
    TUI_ROLE_COUNT
} TuiRole;

enum {
    TUI_STYLE_BOLD = 1 << 0,
    TUI_STYLE_ITALIC = 1 << 1,
    TUI_STYLE_UNDERLINE = 1 << 2,
    TUI_STYLE_REVERSE = 1 << 3,
    TUI_STYLE_DIM = 1 << 4
};

typedef enum {
    TUI_DENSITY_COMPACT = 0,
    TUI_DENSITY_SPACIOUS = 1
} TuiDensity;

typedef struct TuiTheme TuiTheme;

/**
 * tui_theme_create - build a theme from config strings
 * @style: "dark" | "light" | "highcontrast" | "none", case-insensitive;
 *   NULL or unknown values fall back to dark (tui_theme_fell_back() flags).
 * @density: "compact" | "spacious"; NULL or unknown falls back to compact.
 * @accent: RRGGBB hex, optional leading '#', case-insensitive; NULL or
 *   invalid falls back to the preset accent (flagged by fell_back).
 *
 * "none" is the monochrome preset: every role resolves to plain
 * white-on-black (error keeps reverse video so failures stay visible).
 *
 * Return: caller-owned TuiTheme, or NULL on allocation failure. Release
 * with tui_theme_free(). Thread-safe after creation (immutable).
 */
TuiTheme *tui_theme_create(const char *style, const char *density,
                           const char *accent);

/**
 * tui_theme_free - release a theme
 * @t: theme to release, or NULL (no-op).
 *
 * Return: void.
 */
void tui_theme_free(TuiTheme *t);

/**
 * tui_theme_color - resolve a role's 24-bit color (0x00RRGGBB)
 * @t: theme; non-NULL.
 * @role: role to resolve; out-of-range roles return the base foreground.
 *
 * Return: color value. Never fails.
 */
uint32_t tui_theme_color(const TuiTheme *t, TuiRole role);

/**
 * tui_theme_styles - resolve a role's style bits (TUI_STYLE_*)
 * @t: theme; non-NULL.
 * @role: role to resolve.
 *
 * Return: bitmask of TUI_STYLE_* flags. Out-of-range roles return 0.
 */
unsigned tui_theme_styles(const TuiTheme *t, TuiRole role);

/**
 * tui_theme_density - the density mode
 * @t: theme; non-NULL.
 *
 * Return: TUI_DENSITY_COMPACT or TUI_DENSITY_SPACIOUS.
 */
TuiDensity tui_theme_density(const TuiTheme *t);

/**
 * tui_theme_fell_back - did any config value fall back to a default?
 * @t: theme; non-NULL.
 *
 * Callers log this at startup so a typo'd style name is visible in the
 * log file instead of silently applying a different look.
 *
 * Return: 1 when the style name, density name, or accent was invalid.
 */
int tui_theme_fell_back(const TuiTheme *t);

/**
 * tui_theme_spinner_frames - braille spinner frames for the status bar
 * @t: theme; non-NULL.
 * @count: out-param receiving the frame count.
 *
 * Return: static, never-freed array of frames. The renderer substitutes
 * the plain | / - \ set when the terminal lacks Unicode support.
 */
const char *const *tui_theme_spinner_frames(const TuiTheme *t, size_t *count);

/**
 * tui_theme_mix - linear blend of two 24-bit colors
 * @a: first color.
 * @b: second color.
 * @percent_b: percentage of @b in the result, 0-100 (clamped).
 *
 * Used for muted tones (borders, dimmed text). Pure function, exposed for
 * tests.
 *
 * Return: blended color.
 */
uint32_t tui_theme_mix(uint32_t a, uint32_t b, int percent_b);

#endif /* ECHO_TUI_THEME_H */
