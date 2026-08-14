/*
 * tui_theme.c - theme presets and role resolution. All palette literals
 * live here; panes only ever ask for roles. "none" collapses every role to
 * white-on-black (error keeps reverse video). Depends on: strings.h for
 * case-insensitive config matching.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "tui_theme.h"

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

typedef struct {
    uint32_t base_fg;
    uint32_t base_bg;
    uint32_t accent;
    uint32_t error; /* used as foreground under REVERSE, so it must be a
                     * light-on-dark or dark-on-light pair with base_bg */
    uint32_t tool_result; /* tool result blocks */
    uint32_t think; /* <think> block text (muted; distinct from the reply) */
    unsigned backdrop_pct; /* backdrop = base_bg blended toward black */
} Palette;

/* The 0x prefix makes the literals readable next to the plan's hex table:
 * dark   bg #1e1e2e  fg #dcd7ba  accent #7aa2f7  tool #9ece6a  think #9aa5ce
 * light  bg #f2f0e8  fg #2d2a26  accent #2f5fb3  tool #3f6212  think #4c566a
 * high   bg #000000  fg #ffffff  accent #ffd700
 * none   monochrome */
static const Palette PALETTES_DARK = {
    RGB(0xdc, 0xd7, 0xba), RGB(0x1e, 0x1e, 0x2e),
    RGB(0x7a, 0xa2, 0xf7), RGB(0xf2, 0x6d, 0x6d),
    RGB(0x9e, 0xce, 0x6a), RGB(0x9a, 0xa5, 0xce), 50};
static const Palette PALETTES_LIGHT = {
    RGB(0x2d, 0x2a, 0x26), RGB(0xf2, 0xf0, 0xe8),
    RGB(0x2f, 0x5f, 0xb3), RGB(0x8f, 0x1d, 0x1d),
    RGB(0x3f, 0x62, 0x12), RGB(0x4c, 0x56, 0x6a), 30};
static const Palette PALETTES_HIGH = {
    RGB(0xff, 0xff, 0xff), RGB(0x00, 0x00, 0x00),
    RGB(0xff, 0xd7, 0x00), RGB(0xff, 0xff, 0xff),
    RGB(0xff, 0xff, 0xff), RGB(0xff, 0xff, 0xff), 100};
static const Palette PALETTES_NONE = {
    RGB(0xff, 0xff, 0xff), RGB(0x00, 0x00, 0x00),
    RGB(0xff, 0xff, 0xff), RGB(0xff, 0xff, 0xff),
    RGB(0xff, 0xff, 0xff), RGB(0xff, 0xff, 0xff), 100};

struct TuiTheme {
    const Palette *pal; /* points at a static preset */
    uint32_t colors[TUI_ROLE_COUNT];
    unsigned styles[TUI_ROLE_COUNT];
    TuiDensity density;
    int fell_back;
};

static const char *const SPINNER_BRAILLE[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};

static int parse_hex(const char *s, uint32_t *out)
{
    if (!s) return -1;
    if (*s == '#') s++;
    if (strlen(s) != 6) return -1;
    uint32_t v = 0;
    for (int i = 0; i < 6; i++)
    {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return 0;
}

/* Build the role table for a palette: accents, muted borders, inverted
 * status bar. "none" keeps REVERSE for errors but nothing else. */
static void build_roles(TuiTheme *t)
{
    const Palette *p = t->pal;
    int mono = (p == &PALETTES_NONE);

    for (int i = 0; i < TUI_ROLE_COUNT; i++)
    {
        t->styles[i] = 0;
        t->colors[i] = p->base_fg;
    }

    t->colors[TUI_ROLE_BASE_BG] = p->base_bg;
    t->colors[TUI_ROLE_ACCENT] = mono ? p->base_fg : p->accent;
    t->colors[TUI_ROLE_USER] = mono ? p->base_fg : p->accent;
    t->colors[TUI_ROLE_THINK] = mono ? p->base_fg : p->think;
    t->styles[TUI_ROLE_THINK] = mono ? 0 : (TUI_STYLE_DIM | TUI_STYLE_ITALIC);
    t->colors[TUI_ROLE_TOOL] = p->base_fg;
    t->styles[TUI_ROLE_TOOL] = mono ? 0 : TUI_STYLE_DIM;
    t->colors[TUI_ROLE_TOOL_RESULT] = mono ? p->base_fg : p->tool_result;
    t->colors[TUI_ROLE_BACKDROP] = tui_theme_mix(p->base_bg, 0x000000,
                                                 (int)p->backdrop_pct);
    /* REVERSE swaps fg/bg: the error pair must be legible on both sides */
    t->colors[TUI_ROLE_ERROR] = p->error;
    t->styles[TUI_ROLE_ERROR] = TUI_STYLE_REVERSE;
    t->colors[TUI_ROLE_STATUS_BG] = mono ? p->base_fg : p->accent;
    t->colors[TUI_ROLE_STATUS_FG] = p->base_bg;
    t->colors[TUI_ROLE_BORDER] = tui_theme_mix(p->base_fg, p->base_bg, 50);
    t->colors[TUI_ROLE_MODAL_BORDER] = mono ? p->base_fg : p->accent;
    t->styles[TUI_ROLE_STATUS_FG] = mono ? TUI_STYLE_REVERSE : 0;
}

TuiTheme *tui_theme_create(const char *style, const char *density,
                           const char *accent)
{
    TuiTheme *t = calloc(1, sizeof(TuiTheme));
    if (!t) return NULL;

    t->pal = &PALETTES_DARK;
    if (style)
    {
        if (strcasecmp(style, "light") == 0) t->pal = &PALETTES_LIGHT;
        else if (strcasecmp(style, "highcontrast") == 0) t->pal = &PALETTES_HIGH;
        else if (strcasecmp(style, "none") == 0) t->pal = &PALETTES_NONE;
        else if (strcasecmp(style, "dark") != 0) t->fell_back = 1;
    }

    t->density = TUI_DENSITY_COMPACT;
    if (density)
    {
        if (strcasecmp(density, "spacious") == 0) t->density = TUI_DENSITY_SPACIOUS;
        else if (strcasecmp(density, "compact") != 0) t->fell_back = 1;
    }

    if (accent && !(t->pal == &PALETTES_NONE))
    {
        uint32_t custom;
        if (parse_hex(accent, &custom) == 0)
        {
            /* Rebuild the palette copy with a custom accent */
            Palette custom_pal = *t->pal;
            custom_pal.accent = custom;
            /* Point at a heap copy so the palette outlives this call */
            Palette *heap_pal = malloc(sizeof(Palette));
            if (!heap_pal)
            {
                free(t);
                return NULL;
            }
            *heap_pal = custom_pal;
            t->pal = heap_pal;
        }
        else
        {
            t->fell_back = 1;
        }
    }

    build_roles(t);
    return t;
}

void tui_theme_free(TuiTheme *t)
{
    if (!t) return;
    /* Heap palette copies (custom accent) are distinguishable from the
     * static presets by address. */
    if (t->pal != &PALETTES_DARK && t->pal != &PALETTES_LIGHT &&
        t->pal != &PALETTES_HIGH && t->pal != &PALETTES_NONE)
    {
        free((void *)t->pal);
    }
    free(t);
}

uint32_t tui_theme_color(const TuiTheme *t, TuiRole role)
{
    if (!t || role < 0 || role >= TUI_ROLE_COUNT)
        return t ? t->pal->base_fg : 0;
    return t->colors[role];
}

unsigned tui_theme_styles(const TuiTheme *t, TuiRole role)
{
    if (!t || role < 0 || role >= TUI_ROLE_COUNT) return 0;
    return t->styles[role];
}

TuiDensity tui_theme_density(const TuiTheme *t)
{
    return t ? t->density : TUI_DENSITY_COMPACT;
}

int tui_theme_fell_back(const TuiTheme *t)
{
    return t ? t->fell_back : 1;
}

const char *const *tui_theme_spinner_frames(const TuiTheme *t, size_t *count)
{
    (void)t; /* the frame set is preset-independent; future themes may differ */
    if (count) *count = sizeof(SPINNER_BRAILLE) / sizeof(SPINNER_BRAILLE[0]);
    return SPINNER_BRAILLE;
}

uint32_t tui_theme_mix(uint32_t a, uint32_t b, int percent_b)
{
    if (percent_b < 0) percent_b = 0;
    if (percent_b > 100) percent_b = 100;
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8)
    {
        unsigned av = (a >> shift) & 0xff;
        unsigned bv = (b >> shift) & 0xff;
        unsigned m = av + ((bv - av) * (unsigned)percent_b) / 100u;
        out |= (uint32_t)m << shift;
    }
    return out;
}

int tui_theme_plane_colors(const TuiTheme *t, int transparent, TuiRole role,
                           uint32_t *fg, uint32_t *bg)
{
    if (!t || !fg || !bg) return 0;
    if (role < 0 || role >= TUI_ROLE_COUNT)
    {
        *fg = t->pal->base_fg;
        *bg = t->pal->base_bg;
        return 1;
    }
    /* The status bar paints on its own accent field; every other role
     * sits on the base background. */
    uint32_t f = tui_theme_color(t, role);
    uint32_t b = (role == TUI_ROLE_STATUS_FG)
                     ? tui_theme_color(t, TUI_ROLE_STATUS_BG)
                     : tui_theme_color(t, TUI_ROLE_BASE_BG);
    unsigned bits = tui_theme_styles(t, role);
    if (bits & TUI_STYLE_REVERSE)
    {
        uint32_t tmp = f;
        f = b;
        b = tmp;
    }
    if (bits & TUI_STYLE_DIM)
        f = tui_theme_mix(f, b, 60);
    int opaque = !transparent || role == TUI_ROLE_STATUS_FG ||
                 role == TUI_ROLE_ERROR;
    *fg = f;
    *bg = b;
    return opaque;
}
