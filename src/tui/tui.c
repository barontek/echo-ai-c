/*
 * tui.c - the TUI application: notcurses lifecycle, layout, the poll-based
 * main loop (two event wake fds + notcurses keys), event dispatch into the
 * chat/tool/status models, modal handling, and slash commands. All agent
 * mutation goes through the worker; this file never touches the agent
 * except through tui_worker_* calls and the read-only context fields.
 * Depends on: notcurses-core, tui_* modules, change_tracker.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <wchar.h>
#include <wctype.h>

#include <notcurses/notcurses.h>

#include "tui.h"
#include "tui_theme.h"
#include "tui_chat.h"
#include "tui_input.h"
#include "tui_dialogs.h"
#include "tui_worker.h"
#include "markdown.h"
#include "tool_args.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"
#include "../tools/registry.h"
#include "../session/session.h"
#include "../llm/factory.h"

/* Cap for the compacted tool-arguments text shown on header lines. */
#define TOOL_ARGS_MAX 200

/* What a deferred menu action runs once the picker has closed (blocking
 * prompt flows cannot nest inside the modal ANSWERED handling). */
enum {
    MENU_ACTION_NONE = 0,
    MENU_ACTION_CHANGE_PASSWORD
};

/* What the Ctrl-P picker will commit: a provider choice triggers a
 * provider switch + model fetch; a model choice triggers a model switch. */
enum {
    PICKER_NONE = 0,
    PICKER_PROVIDER,
    PICKER_MODEL,
    PICKER_MENU,          /* main menu: pick the next action */
    PICKER_EFFORT,        /* reasoning-effort levels for the current provider */
    PICKER_THEME,         /* theme presets */
    PICKER_SESSION_LOAD,  /* session list -> switch */
    PICKER_SESSION_DELETE, /* session list -> delete (then confirm) */
    PICKER_SESSION_EXPORT, /* session list -> export (path input) */
    PICKER_SESSION_RENAME  /* session list -> rename (name input) */
};

/* What the collected text of a menu-opened input modal does. */
enum {
    INPUT_NONE = 0,
    INPUT_EXPORT_PATH,
    INPUT_RENAME_NAME
};

/* What a menu-opened confirm modal commits. */
enum {
    CONFIRM_NONE = 0,
    CONFIRM_DELETE
};

/* Forward decls: handle_event (earlier in the file) opens the pickers. */
static void open_provider_picker(TuiApp *app);
static void open_model_picker(TuiApp *app);
static int picker_commit(TuiApp *app);
static void rebuild_chat(TuiApp *app, const char *json);
static void slash_theme(TuiApp *app, const char *style);
static void slash_change_password(TuiApp *app);

struct TuiApp {
    TuiAppCtx ctx;
    struct notcurses *nc;
    struct ncplane *status_plane;
    struct ncplane *chat_plane;
    struct ncplane *tools_plane;
    struct ncplane *input_plane;
    struct ncplane *footer_plane;
    struct ncplane *prompt_plane;  /* approval bar above the input line */
    struct ncplane *modal_plane;
    struct ncplane *backdrop_plane;
    TuiTheme *theme;
    int transparent; /* 1: never paint the base background (see-through) */
    TuiChat *chat;
    TuiInput *input;
    TuiModal *modal;
    TuiWorker *worker;
    char *model;      /* status bar model (owned) */
    char *provider;   /* status bar provider (owned) */
    char *session_id; /* status bar session id (owned) */
    char *title;      /* generated session title (owned); shown in status */
    char *status_msg; /* transient status message (owned) */
    char *active_tool; /* currently running tool (owned); NULL when idle */
    char *active_tool_args; /* compact args of the running tool (owned) */
    char **pending_models;   /* fetched model names for the picker (owned) */
    int pending_models_count;
    int picker_context;      /* PICKER_*: what the open picker commits */
    char *menu_session_id;   /* session picked via the menu (owned) */
    int input_action;        /* INPUT_*: what the open input modal collects */
    int confirm_action;      /* CONFIRM_*: what the open confirm modal commits */
    int menu_action;         /* MENU_ACTION_*: deferred blocking menu action */
    char *log_path;   /* owned */
    size_t chat_top;  /* scroll offset in lines */
    int auto_scroll;
    unsigned input_rows;  /* input plane height (multiline growth) */
    unsigned input_rows_laid; /* input height the current planes were built for */
    size_t frame;     /* spinner frame counter */
    int quit;
    unsigned term_rows, term_cols;
    struct sigaction saved_sigs[2]; /* SIGINT/SIGTERM, restored on destroy */
};

/* Display width of the first n bytes of a UTF-8 string (wcwidth-based). */
static size_t utf8_width(const char *s, size_t n)
{
    size_t i = 0;
    size_t w = 0;
    while (i < n)
    {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; len = 2; }
        else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; len = 3; }
        else { cp = c & 0x07; len = 4; }
        if (i + (size_t)len > n) break;
        for (int k = 1; k < len; k++)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3f);
        int cw = wcwidth((wchar_t)cp);
        w += cw > 0 ? (size_t)cw : 0;
        i += (size_t)len;
    }
    return w;
}

/* ---- signal handling: graceful exit on SIGINT/SIGTERM ---- */

static volatile sig_atomic_t g_sig_quit = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_sig_quit = 1; /* async-signal-safe: a flag is all we touch here */
}

static int install_signal_handlers(struct sigaction *saved, size_t n_saved)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    /* No SA_RESTART: poll() returns EINTR so the loop sees the flag
     * immediately instead of sleeping out its timeout. */
    const int sigs[] = {SIGINT, SIGTERM};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]) && i < n_saved; i++)
    {
        if (sigaction(sigs[i], &sa, &saved[i]) != 0)
            return -1;
    }
    return 0;
}

static void restore_signal_handlers(const struct sigaction *saved,
                                    size_t n_saved)
{
    const int sigs[] = {SIGINT, SIGTERM};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]) && i < n_saved; i++)
        (void)sigaction(sigs[i], &saved[i], NULL);
}

/* Fold a pending signal into the app's quit state and cancel any run. */
static void check_signals(TuiApp *app)
{
    if (g_sig_quit && !app->quit)
    {
        app->quit = 1;
        if (app->worker) tui_worker_cancel(app->worker);
    }
}

/* ---- stderr redirect ---- */

static char *default_log_path(void)
{
    const char *home = getenv("HOME");
    if (!home) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/.config/echo-ai/tui.log", home) < 0) return NULL;
    return path;
}

static int redirect_stderr(const char *path)
{
    FILE *f = freopen(path, "a", stderr);
    if (!f)
    {
        log_error("tui: stderr redirect failed", "path", path, NULL);
        return -1;
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    return 0;
}

/* ---- layout ---- */

static void destroy_planes(TuiApp *app)
{
    struct ncplane *planes[] = {
        app->status_plane, app->chat_plane, app->tools_plane,
        app->input_plane, app->footer_plane, app->prompt_plane,
        app->modal_plane, app->backdrop_plane
    };
    for (size_t i = 0; i < sizeof(planes) / sizeof(planes[0]); i++)
    {
        if (planes[i]) ncplane_destroy(planes[i]);
    }
    app->status_plane = app->chat_plane = app->tools_plane = NULL;
    app->input_plane = app->footer_plane = app->prompt_plane = NULL;
    app->modal_plane = app->backdrop_plane = NULL;
}

static struct ncplane *make_plane(TuiApp *app, unsigned rows, unsigned cols,
                                  int y, int x)
{
    ncplane_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.y = y;
    opts.x = x;
    opts.rows = rows;
    opts.cols = cols;
    return ncplane_create(notcurses_stdplane(app->nc), &opts);
}

/* Wrapped height of the approval prompt bar (defined with the modal
 * rendering below; layout needs it first). */
static int approval_bar_lines(const TuiApp *app, unsigned cols);

/* Rebuild the plane stack when the terminal size changes — or when the
 * approval bar appears/disappears (callers invalidate term_rows). */
static int layout_planes(TuiApp *app)
{
    unsigned rows, cols;
    notcurses_term_dim_yx(app->nc, &rows, &cols); /* void in 3.0.17 */
    int bar = approval_bar_lines(app, cols);
    /* Cap the bar so the chat pane keeps at least one row on short terms. */
    if (bar > (int)rows - 5) bar = (int)rows - 5;
    if (bar < 0) bar = 0;
    /* The multiline input pane grows with the wrapped text; recompute the
     * layout whenever its height or the terminal size changes. */
    if (rows == app->term_rows && cols == app->term_cols &&
        app->input_rows == app->input_rows_laid && app->status_plane)
        return 0;
    app->term_rows = rows;
    app->term_cols = cols;
    app->input_rows_laid = app->input_rows;
    destroy_planes(app);

    if (rows < 6 || cols < 20)
        return -1; /* too small to be usable; caller exits cleanly */
    if (app->input_rows == 0) app->input_rows = 1;
    if (app->input_rows > 5) app->input_rows = 5;
    if (app->input_rows + 5 > rows) app->input_rows = rows > 5 ? rows - 5 : 1;
    app->status_plane = make_plane(app, 1, cols, 0, 0);
    app->tools_plane = make_plane(app, 1, cols, 1, 0);
    /* chat shrinks by the approval bar height while it is up */
    int chat_h = (int)rows - 3 - bar - (int)app->input_rows;
    if (chat_h < 1) chat_h = 1;
    app->chat_plane = make_plane(app, (unsigned)chat_h, cols, 2, 0);
    if (bar)
        app->prompt_plane = make_plane(app, (unsigned)bar, cols,
                                       (int)rows - 2 - bar -
                                           (int)app->input_rows + 1, 0);
    app->input_plane = make_plane(app, app->input_rows, cols,
                                  (int)rows - 1 - (int)app->input_rows, 0);
    app->footer_plane = make_plane(app, 1, cols, (int)rows - 1, 0);
    app->modal_plane = NULL;
    app->backdrop_plane = NULL;
    if (!app->status_plane || !app->chat_plane || !app->tools_plane ||
        !app->input_plane || !app->footer_plane)
        return -1;
    return 0;
}

/* notcurses normalizes ctrl+letter to the uppercased letter with the
 * CTRL modifier (legacy control bytes like 0x03 are remapped the same
 * way, and the kitty keyboard protocol reports them identically), so
 * control bindings must match the letter + modifier — a raw control
 * code check is dead code on every terminal. */
static int key_is_ctrl(const ncinput *ni, uint32_t letter)
{
    return ni->id == letter &&
           (ni->modifiers & NCKEY_MOD_CTRL) != 0;
}

/* Render a ucs32 codepoint as UTF-8 into out (room for 4 bytes). */
static size_t cp_to_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Text a key event should insert. Under the kitty keyboard protocol the
 * event id is the base key (shift applied) and modifier-produced
 * characters (AltGr layouts, dead keys, IME) arrive in eff_text —
 * prefer it when it differs from the id. */
static size_t ncinput_text(const ncinput *ni, char *out, size_t cap)
{
    if (ni->eff_text[0] != 0 &&
        (ni->eff_text[0] != ni->id || ni->id < 0x20))
    {
        size_t o = 0;
        for (unsigned i = 0;
             i < NCINPUT_MAX_EFF_TEXT_CODEPOINTS && ni->eff_text[i] != 0; i++)
        {
            if (o + 4 >= cap) break; /* keep room for the NUL */
            o += cp_to_utf8(ni->eff_text[i], out + o);
        }
        out[o] = '\0';
        if (o > 0) return o;
    }
    return strlcpy(out, ni->utf8, cap);
}

/* First codepoint a key event types (for single-character consumers
 * like the modal dispatch). */
static int ncinput_cp(const ncinput *ni)
{
    if (ni->eff_text[0] != 0 && ni->eff_text[0] != ni->id)
        return (int)ni->eff_text[0];
    return (int)ni->id;
}

/* Blocking-modal loops (password/notice) run before the main loop and
 * must handle a too-small terminal the same way app_loop does: wait for
 * a resize instead of rendering into NULL planes. Returns 0 when a
 * resize arrived, -1 on EOF or Ctrl-C (app->quit set). */
static int wait_for_terminal(TuiApp *app)
{
    ncinput ni;
    uint32_t rc;
    while ((rc = notcurses_get_nblock(app->nc, &ni)) != 0 && rc != UINT32_MAX)
    {
        if (ni.evtype == NCTYPE_RELEASE) continue; /* kitty prot. */
        if (ni.id == NCKEY_RESIZE) return 0;
        if (key_is_ctrl(&ni, 'C')) { app->quit = 1; return -1; } /* Ctrl-C aborts */
    }
    if (rc == UINT32_MAX) { app->quit = 1; return -1; } /* EOF: terminal gone */
    check_signals(app);
    return app->quit ? -1 : 0;
}

/* ---- rendering helpers ---- */

/* notcurses 3.0.17 has no REVERSE/DIM styles; simulate them with color:
 * REVERSE swaps fg/bg, DIM mixes the fg toward the background. The status
 * bar role inverts onto its own accent background. In transparent mode
 * the base background is never painted (the plane stays see-through so
 * kitty's transparency/blur shows), but the status bar's accent field,
 * the error highlight, and the modal backdrop keep their opaque
 * backgrounds — they are design elements, not text backdrops. Roles
 * without an opaque background reset the plane's background to default:
 * cells inherit the plane's channels at write time, so without the reset
 * a previous error/status paint would leak its background into every
 * cell written after it. */
static void plane_color(TuiApp *app, struct ncplane *p, TuiRole role)
{
    uint32_t fg;
    uint32_t bg;
    int opaque = tui_theme_plane_colors(app->theme, app->transparent, role,
                                        &fg, &bg);
    (void)ncplane_set_fg_rgb8(p, (fg >> 16) & 0xff, (fg >> 8) & 0xff, fg & 0xff);
    if (opaque)
    {
        (void)ncplane_set_bg_rgb8(p, (bg >> 16) & 0xff,
                                  (bg >> 8) & 0xff, bg & 0xff);
    }
    else
    {
        (void)ncplane_set_bg_default(p);
    }

    unsigned bits2 = tui_theme_styles(app->theme, role);
    unsigned ncstyle = 0;
    if (bits2 & TUI_STYLE_BOLD) ncstyle |= NCSTYLE_BOLD;
    if (bits2 & TUI_STYLE_ITALIC) ncstyle |= NCSTYLE_ITALIC;
    if (bits2 & TUI_STYLE_UNDERLINE) ncstyle |= NCSTYLE_UNDERLINE;
    ncplane_set_styles(p, ncstyle);
}

/* Strip <think>/</think> markers out of a segment (write into out). */
static size_t strip_markers(const char *s, size_t len, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < len;)
    {
        if (len - i >= 7 && strncmp(s + i, "<think>", 7) == 0) i += 7;
        else if (len - i >= 8 && strncmp(s + i, "</think>", 8) == 0) i += 8;
        else out[o++] = s[i++];
    }
    out[o] = '\0';
    return o;
}

static TuiRole kind_role(TuiBlockKind kind)
{
    switch (kind)
    {
    case TUI_BLOCK_USER: return TUI_ROLE_USER;
    case TUI_BLOCK_THINK: return TUI_ROLE_THINK;
    case TUI_BLOCK_TOOL: return TUI_ROLE_TOOL_RESULT;
    case TUI_BLOCK_ERROR: return TUI_ROLE_ERROR;
    default: return TUI_ROLE_ASSISTANT;
    }
}

/* Role label for the block header line (opencode-style message blocks). */
static const char *kind_label(TuiBlockKind kind)
{
    switch (kind)
    {
    case TUI_BLOCK_USER: return "you";
    case TUI_BLOCK_THINK: return "thinking";
    case TUI_BLOCK_TOOL: return "tool";
    case TUI_BLOCK_ERROR: return "error";
    default: return "echo";
    }
}

/* ---- markdown-aware line rendering ---- */

static int markdown_kind(TuiBlockKind kind)
{
    return kind == TUI_BLOCK_USER || kind == TUI_BLOCK_ASSISTANT ||
           kind == TUI_BLOCK_THINK;
}

/* Map MdStyle bits onto a notcurses style + fg (bg stays as the block
 * role's — see-through in transparent mode). Resets leftover styles.
 * Role-level styles (think = dim + italic) apply beneath the inline
 * markdown styles: this is the only painter for message content lines,
 * so without them a think block would render exactly like the reply. */
static void md_apply(TuiApp *app, struct ncplane *p, TuiRole role,
                     unsigned bits)
{
    uint32_t fg = tui_theme_color(app->theme, role);
    unsigned ncstyle = 0;
    unsigned role_bits = tui_theme_styles(app->theme, role);
    if (role_bits & TUI_STYLE_DIM)
        fg = tui_theme_mix(fg, tui_theme_color(app->theme, TUI_ROLE_BASE_BG), 60);
    if (role_bits & TUI_STYLE_ITALIC) ncstyle |= NCSTYLE_ITALIC;
    if (role_bits & TUI_STYLE_BOLD) ncstyle |= NCSTYLE_BOLD;
    if (bits & MD_STYLE_BOLD) ncstyle |= NCSTYLE_BOLD;
    if (bits & MD_STYLE_ITALIC) ncstyle |= NCSTYLE_ITALIC;
    if (bits & MD_STYLE_STRIKE) ncstyle |= NCSTYLE_STRUCK;
    if (bits & MD_STYLE_LINK) ncstyle |= NCSTYLE_UNDERLINE;
    if (bits & MD_STYLE_CODE)
        fg = tui_theme_color(app->theme, TUI_ROLE_TOOL_RESULT);
    if (bits & MD_STYLE_HEADING)
    {
        fg = tui_theme_color(app->theme, TUI_ROLE_ACCENT);
        ncstyle |= NCSTYLE_BOLD;
    }
    if (bits & MD_STYLE_DIM)
        fg = tui_theme_mix(fg, tui_theme_color(app->theme, TUI_ROLE_BASE_BG), 60);
    (void)ncplane_set_fg_rgb8(p, (fg >> 16) & 0xff, (fg >> 8) & 0xff, fg & 0xff);
    (void)ncplane_set_styles(p, ncstyle);
}

/* Block header line: rail + role label. Tool blocks show the tool name
 * bold with the compact call arguments dimmed after it ("bash: ls -la");
 * without arguments they keep the "<name> tool" label. Names and args
 * are model-provided, so the whole line is clamped to the content width
 * at a codepoint boundary. */
static void put_block_header(TuiApp *app, struct ncplane *p, TuiRole role,
                             size_t cw, TuiBlockKind bk, int row,
                             const char *title, const char *args)
{
    const char *label;
    char label_buf[64];
    int show_args = 0;
    if (bk == TUI_BLOCK_TOOL && title)
    {
        /* Friendly label ("Read") over the raw tool name; the raw name
         * stays the block title so tool_end matches pending blocks. */
        label = tool_args_label(title);
        if (args && args[0] != '\0')
        {
            show_args = 1;
        }
        else
        {
            snprintf(label_buf, sizeof(label_buf), "%s tool", label);
            label = label_buf;
        }
    }
    else
    {
        label = kind_label(bk);
    }

    char name_buf[64];
    (void)tui_chat_truncate_width(label, cw, name_buf, sizeof(name_buf));
    (void)ncplane_cursor_move_yx(p, row, 0);
    plane_color(app, p, role);
    (void)ncplane_putstr(p, "\xE2\x94\x82"); /* │ */
    (void)ncplane_cursor_move_yx(p, row, 2);
    (void)ncplane_set_styles(p, NCSTYLE_BOLD);
    (void)ncplane_putstr(p, name_buf);
    if (show_args)
    {
        size_t used = tui_chat_display_width(name_buf, strlen(name_buf));
        if (used + 2 < cw)
        {
            char args_buf[TOOL_ARGS_MAX + 4];
            (void)tui_chat_truncate_width(args, cw - used - 2, args_buf,
                                          sizeof(args_buf));
            if (args_buf[0] != '\0')
            {
                (void)ncplane_set_styles(p, 0);
                md_apply(app, p, role, MD_STYLE_DIM);
                (void)ncplane_putstr(p, ": ");
                (void)ncplane_putstr(p, args_buf);
            }
        }
    }
    (void)ncplane_set_styles(p, 0);
}

/* Horizontal rule: a dim line of box-drawing fills the content width. */
static void put_hr(TuiApp *app, struct ncplane *p, TuiRole role,
                   char *buf, size_t bufcap, size_t cw)
{
    md_apply(app, p, role, MD_STYLE_DIM);
    size_t o = 0;
    while (o + 3 < bufcap && o < cw * 3)
    {
        buf[o++] = '\xE2';
        buf[o++] = '\x94';
        buf[o++] = '\x80';
    }
    buf[o] = '\0';
    (void)ncplane_putstr(p, buf);
}

/* One content line with markdown applied (the rail and cursor are
 * already set up; the cursor sits at col 2). Line-level kinds set a
 * whole-line style; inline runs refine it run by run. */
static void put_markdown_line(TuiApp *app, struct ncplane *p, TuiRole role,
                              const char *seg, size_t seg_len, MdLineKind lk,
                              int in_fence, MdRun *runs, size_t run_cap)
{    if (in_fence && lk != MD_LINE_CODE_FENCE)
    {
        /* fenced code is literal: nothing inside it is a delimiter */
        md_apply(app, p, role, MD_STYLE_CODE);
        (void)ncplane_putnstr(p, seg_len, seg);
        return;
    }
    unsigned line_bits = 0;
    if (lk == MD_LINE_HEADING) line_bits |= MD_STYLE_HEADING;
    else if (lk == MD_LINE_QUOTE || lk == MD_LINE_CODE_FENCE)
        line_bits |= MD_STYLE_DIM;

    /* Headings consume their whole marker (indent, hashes, one space)
     * like CommonMark does; only the heading text is drawn. */
    const char *base = seg;
    size_t base_len = seg_len;
    if (lk == MD_LINE_HEADING)
    {
        size_t skip = 0;
        while (skip < base_len && base[skip] == ' ') skip++;
        while (skip < base_len && base[skip] == '#') skip++;
        if (skip < base_len && base[skip] == ' ') skip++;
        if (skip >= base_len) return; /* bare "#": an empty heading */
        base += skip;
        base_len -= skip;
    }

    size_t n = md_parse_inline(base, base_len, runs, run_cap);
    if (n == 0)
    {
        md_apply(app, p, role, line_bits);
        (void)ncplane_putnstr(p, base_len, base);
        return;
    }
    for (size_t i = 0; i < n; i++)
    {
        size_t start = runs[i].start;
        size_t len = runs[i].len;
        if (start >= base_len) continue;
        if (start + len > base_len) len = base_len - start;
        md_apply(app, p, role, line_bits | runs[i].style);
        (void)ncplane_putnstr(p, len, base + start);
    }
    (void)ncplane_set_styles(p, 0); /* leave no style on the plane */
}

/* Diff-colored tool-result line: added lines green, removed lines red,
 * context dimmed (the classifier runs before this, so the shape is
 * known). Returns 1 when the line was drawn. */
static int put_diff_line(TuiApp *app, struct ncplane *p, TuiRole role,
                         TuiDiffKind kind, const char *seg, size_t seg_len)
{
    uint32_t fg;
    switch (kind)
    {
    case TUI_DIFF_ADD:
        fg = tui_theme_color(app->theme, TUI_ROLE_DIFF_ADD);
        break;
    case TUI_DIFF_DEL:
        fg = tui_theme_color(app->theme, TUI_ROLE_DIFF_DEL);
        break;
    case TUI_DIFF_CONTEXT:
        fg = tui_theme_mix(tui_theme_color(app->theme, role),
                           tui_theme_color(app->theme, TUI_ROLE_BASE_BG), 60);
        break;
    default:
        return 0;
    }
    (void)ncplane_set_styles(p, 0);
    (void)ncplane_set_fg_rgb8(p, (fg >> 16) & 0xff, (fg >> 8) & 0xff,
                              fg & 0xff);
    (void)ncplane_putnstr(p, seg_len, seg);
    return 1;
}

static void render_chat(TuiApp *app)
{
    struct ncplane *p = app->chat_plane;
    unsigned rows, cols;
    ncplane_dim_yx(p, &rows, &cols);
    ncplane_erase(p);

    /* Left rail column (role color) + padding: content wraps at cols-3 */
    size_t cw = cols >= 6 ? (size_t)cols - 3 : (size_t)cols;
    size_t total = tui_chat_total_lines(app->chat, cw);
    if (app->auto_scroll && total > (size_t)rows)
        app->chat_top = total - (size_t)rows;
    size_t top = app->chat_top;

    size_t virtual_line = 0;
    size_t drawn = 0;
    /* One scratch segment for the whole pass (strip_markers output is
     * never longer than its input line); marker stripping happens per
     * drawn line. */
    char *seg = malloc(cw * 4 + 1);
    char *row_buf = malloc(cw * 8 + 1);
    MdRun *runs = malloc((cw * 4 + 2) * sizeof(MdRun));
    if (!seg || !row_buf || !runs)
    {
        free(seg);
        free(row_buf);
        free(runs);
        return;
    }
    for (size_t b = 0; b < tui_chat_block_count(app->chat) && drawn < (size_t)rows; b++)
    {
        TuiBlockKind bk = tui_chat_block_kind(app->chat, b);
        TuiRole role = kind_role(bk);
        const char *text = tui_chat_block_text(app->chat, b);
        size_t lines = 0;
        const size_t *starts = tui_chat_block_line_starts(app->chat, b, cw,
                                                          &lines);

        /* Long tool blocks collapse to the threshold with a marker row
         * that the click handler toggles. Only TOOL blocks ever get the
         * marker — other kinds always render their full content. */
        int truncated = tui_chat_block_effective_collapsed(app->chat, b, cw);
        size_t content = truncated ? TUI_CHAT_COLLAPSE_THRESHOLD : lines;
        int has_marker = (bk == TUI_BLOCK_TOOL && lines > TUI_CHAT_COLLAPSE_THRESHOLD);

        /* Header line: rail + role label. Tool blocks show the tool name
         * bold with the compact call arguments dimmed ("bash: ls -la"),
         * or "<name> tool" when the call carried no arguments. */
        if (virtual_line >= top && drawn < (size_t)rows)
        {
            put_block_header(app, p, role, cw, bk, (int)drawn,
                             tui_chat_block_title(app->chat, b),
                             tui_chat_block_args(app->chat, b));
            drawn++;
        }
        virtual_line++;

        /* A pending tool block (empty text) shows the spinner animation
         * in place of content while its tool runs. */
        int pending = (bk == TUI_BLOCK_TOOL && text[0] == '\0');

        /* Markdown (headings, emphasis, tables, code fences) applies to
         * message text, not to tool results or errors. Fence state is
         * per-block and recomputed every frame: the chat model is
         * re-rendered in full, so no state survives between frames. */
        int markdown_block = markdown_kind(bk);
        int fence = 0;
        for (size_t l = 0; l < content; l++)
        {
            size_t seg_len = 0;
            MdLineKind lk = MD_LINE_PLAIN;
            MdTable *tbl = NULL;
            size_t tbl_rows = 0;
            if (starts)
            {
                size_t s = starts[l];
                size_t e = starts[l + 1];
                if (e > s)
                {
                    size_t cap = cw * 4;
                    if (e - s > cap) e = s + cap; /* pathological combining-mark runs */
                    seg_len = strip_markers(text + s, e - s, seg);
                }
                if (markdown_block && !pending)
                {
                    lk = md_line_kind(seg, seg_len);
                    if (lk == MD_LINE_CODE_FENCE)
                        fence = !fence;
                    /* A row only starts a table when the next line is a
                     * separator; a lone row renders as plain text. */
                    if (lk == MD_LINE_TABLE_ROW && !fence &&
                        l + 1 < content)
                    {
                        size_t s2 = starts[l + 1];
                        size_t e2 = starts[l + 2];
                        size_t slen = 0;
                        if (e2 > s2)
                        {
                            size_t cap2 = cw * 4;
                            if (e2 - s2 > cap2) e2 = s2 + cap2;
                            slen = strip_markers(text + s2, e2 - s2, row_buf);
                        }
                        if (md_line_kind(row_buf, slen) == MD_LINE_TABLE_SEP)
                            tbl = md_table_scan(text, starts, content, l,
                                                &tbl_rows);
                    }
                }
            }

            if (tbl)
            {
                /* Render every row (padded to the column widths), then
                 * consume the separator as a blank line; the outer loop
                 * must skip both so scroll math stays exact. */
                for (size_t r = 0; r < tbl_rows; r++)
                {
                    /* rows are non-contiguous: the separator at l + 1
                     * sits between the first row and the rest */
                    size_t rline = (r == 0) ? l : l + r + 1;
                    if (virtual_line >= top && drawn < (size_t)rows)
                    {
                        size_t rs = starts[rline];
                        size_t re = starts[rline + 1];
                        size_t rlen = 0;
                        if (re > rs)
                        {
                            size_t cap3 = cw * 4;
                            if (re - rs > cap3) re = rs + cap3;
                            rlen = strip_markers(text + rs, re - rs, seg);
                        }
                        (void)ncplane_cursor_move_yx(p, (int)drawn, 0);
                        plane_color(app, p, role);
                        (void)ncplane_putstr(p, "\xE2\x94\x82"); /* │ */
                        (void)ncplane_cursor_move_yx(p, (int)drawn, 2);
                        size_t n = md_table_render_row(row_buf, cw * 8 + 1,
                                                       seg, rlen, tbl);
                        (void)ncplane_putnstr(p, n, row_buf);
                        drawn++;
                    }
                    virtual_line++;
                }
                virtual_line++;
                if (virtual_line >= top && drawn < (size_t)rows)
                    drawn++;
                md_table_free(tbl);
                l += tbl_rows; /* the loop's l++ clears the separator too */
                continue;
            }

            if (virtual_line >= top && drawn < (size_t)rows)
            {
                (void)ncplane_cursor_move_yx(p, (int)drawn, 0);
                plane_color(app, p, role);
                (void)ncplane_putstr(p, "\xE2\x94\x82"); /* │ */
                (void)ncplane_cursor_move_yx(p, (int)drawn, 2);
                if (pending)
                {
                    size_t frames = 0;
                    const char *const *sp =
                        tui_theme_spinner_frames(app->theme, &frames);
                    if (frames > 0)
                        (void)ncplane_putnstr(p, 1,
                                              sp[app->frame % frames]);
                }
                else if (starts)
                {
                    if (markdown_block && lk == MD_LINE_HR && !fence)
                        put_hr(app, p, role, row_buf, cw * 8 + 1, cw);
                    else if (markdown_block)
                        put_markdown_line(app, p, role, seg, seg_len, lk,
                                          fence, runs, cw * 4 + 2);
                    else if (bk == TUI_BLOCK_TOOL &&
                             put_diff_line(app, p, role,
                                           tui_diff_line_kind(seg, seg_len),
                                           seg, seg_len))
                        ;
                    else
                        (void)ncplane_putnstr(p, seg_len, seg);
                }
                drawn++;
            }
            virtual_line++;
        }
        if (has_marker && virtual_line >= top && drawn < (size_t)rows)
        {
            char marker[96];
            if (truncated)
            {
                snprintf(marker, sizeof(marker),
                         "  \xE2\x80\xA6 +%zu more lines \xE2\x80\x94 click to expand",
                         lines - TUI_CHAT_COLLAPSE_THRESHOLD);
            }
            else
            {
                snprintf(marker, sizeof(marker),
                         "  \xE2\x80\x94 click to collapse");
            }
            (void)ncplane_cursor_move_yx(p, (int)drawn, 0);
            plane_color(app, p, role);
            (void)ncplane_putstr(p, "\xE2\x94\x82"); /* │ */
            (void)ncplane_cursor_move_yx(p, (int)drawn, 2);
            (void)ncplane_putnstr(p, strlen(marker), marker);
            drawn++;
        }
        virtual_line += has_marker ? 1 : 0;
        /* One separator line after every block: it must consume a screen
         * row (left blank) or blocks render adjacent — the scroll math
         * counts it, so the drawing must too. */
        virtual_line++;
        if (virtual_line >= top && drawn < (size_t)rows)
            drawn++;
    }
    free(seg);
    free(row_buf);
    free(runs);
}

static void render_status(TuiApp *app)
{
    struct ncplane *p = app->status_plane;
    unsigned cols;
    ncplane_dim_yx(p, NULL, &cols);
    ncplane_erase(p);
    plane_color(app, p, TUI_ROLE_STATUS_FG);

    char line[512];
    int n = snprintf(line, sizeof(line), " %s | %s | session: %s%s%s%s | tools: %d",
                     app->model ? app->model : "?",
                     app->provider ? app->provider : "?",
                     app->session_id ? app->session_id : "none",
                     app->title ? " (" : "",
                     app->title ? app->title : "",
                     app->title ? ")" : "",
                     app->ctx.tool_count);
    /* snprintf returns the would-be length on truncation: clamping keeps
     * the chained writes inside the buffer (titles come from the model
     * and are unbounded). */
    if (n < 0) return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;
    if (tui_worker_busy(app->worker))
    {
        size_t frames = 0;
        const char *const *sp = tui_theme_spinner_frames(app->theme, &frames);
        if (frames > 0)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s",
                          sp[app->frame % frames]);
    }
    if (n < 0) return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;
    if (app->status_msg && app->status_msg[0])
        n += snprintf(line + n, sizeof(line) - (size_t)n, "  [%s]", app->status_msg);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;
    (void)ncplane_putnstr(p, (size_t)n, line);
}

static void render_tools(TuiApp *app)
{
    struct ncplane *p = app->tools_plane;
    unsigned cols;
    ncplane_dim_yx(p, NULL, &cols);
    ncplane_erase(p);
    plane_color(app, p, TUI_ROLE_TOOL_RESULT);

    /* Live-activity strip: shows only the currently running tool and
     * collapses when idle. Tool history lives in the chat blocks. */
    if (!app->active_tool) return;

    char line[320];
    int n;
    if (app->active_tool_args && app->active_tool_args[0])
        n = snprintf(line, sizeof(line), "%s: %s", app->active_tool,
                     app->active_tool_args);
    else
        n = snprintf(line, sizeof(line), "%s", app->active_tool);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1; /* truncated: keep chained writes in bounds */
    n += snprintf(line + n, sizeof(line) - (size_t)n,
                  "  \xE2\x80\xA6 running");
    if (tui_worker_busy(app->worker))
    {
        size_t frames = 0;
        const char *const *sp = tui_theme_spinner_frames(app->theme, &frames);
        if (frames > 0)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s",
                          sp[app->frame % frames]);
    }
    if (n < 0) return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;
    size_t len = (size_t)n;
    if (len > (size_t)cols - 1) len = (size_t)cols - 1;
    (void)ncplane_putnstr(p, len, line);
}

static void render_input(TuiApp *app)
{
    struct ncplane *p = app->input_plane;
    unsigned rows, cols;
    ncplane_dim_yx(p, &rows, &cols);
    ncplane_erase(p);
    plane_color(app, p, TUI_ROLE_BASE_FG);
    const char *text = tui_input_text(app->input);
    size_t len = strlen(text);
    size_t width = cols > 2 ? (size_t)cols - 2 : (size_t)cols;

    /* The pane grows to the wrapped line count (capped); a change marks
     * the layout stale so the next frame rebuilds the planes. */
    size_t wrapped = tui_chat_wrap(text, width, NULL, 0);
    if (wrapped == 0) wrapped = 1;
    unsigned want = wrapped > 5 ? 5U : (unsigned)wrapped;
    if (want != app->input_rows)
    {
        app->input_rows = want;
        app->term_rows = 0; /* force layout_planes on the next frame */
    }

    size_t *starts = malloc((wrapped + 1) * sizeof(size_t));
    size_t cursor_off = tui_input_cursor(app->input);
    size_t cursor_line = 0;
    size_t cursor_col = 0;
    if (starts)
    {
        (void)tui_chat_wrap(text, width, starts, wrapped + 1);
        starts[wrapped] = len;
        for (size_t l = 0; l < wrapped; l++)
        {
            if (cursor_off >= starts[l] &&
                cursor_off <= starts[l + 1])
            {
                cursor_line = l;
                cursor_col = utf8_width(text + starts[l],
                                        cursor_off - starts[l]);
                break;
            }
        }
    }
    /* Scroll the view so the cursor's line stays visible; the editor
     * cursor normally sits on the last line, i.e. show the last rows. */
    size_t first = cursor_line >= rows ? cursor_line - rows + 1 : 0;
    if (starts)
    {
        for (size_t l = first; l < wrapped && l < first + rows; l++)
        {
            size_t s = starts[l];
            size_t e = starts[l + 1];
            size_t shown = e > s ? e - s : 0;
            if (shown > width) shown = width;
            (void)ncplane_cursor_move_yx(p, (int)(l - first), 0);
            if (l == 0) (void)ncplane_putstr(p, "> ");
            else (void)ncplane_putstr(p, "  ");
            (void)ncplane_putnstr(p, shown, text + s);
        }
        free(starts);
    }
    if (cursor_col > width) cursor_col = width;
    (void)ncplane_cursor_move_yx(p, (int)(cursor_line - first), 2 + (int)cursor_col);
    (void)ncplane_putnstr(p, 1, " ");
    (void)ncplane_cursor_move_yx(p, 0, 0);
}

static void render_footer(TuiApp *app)
{
    struct ncplane *p = app->footer_plane;
    unsigned cols;
    ncplane_dim_yx(p, NULL, &cols);
    ncplane_erase(p);
    plane_color(app, p, TUI_ROLE_BORDER);
    const char *hints = " Enter send | Shift+Enter newline | Esc cancel | Ctrl-P menu | Ctrl-C quit ";
    size_t n = strlen(hints);
    if (n > (size_t)cols - 1) n = (size_t)cols - 1;
    (void)ncplane_putnstr(p, n, hints);
}

static const char *modal_hint(TuiModalKind kind)
{
    switch (kind)
    {
    case TUI_MODAL_APPROVAL: return "y allow \xE2\x80\xA2 n deny";
    case TUI_MODAL_CONFIRM_QUIT: return "y quit \xE2\x80\xA2 any other key cancels";
    case TUI_MODAL_PASSWORD: return "enter submit \xE2\x80\xA2 esc cancel";
    case TUI_MODAL_ASK_USER: return "enter submit \xE2\x80\xA2 esc cancel";
    case TUI_MODAL_PICKER: return "type to filter \xE2\x80\xA2 up/down move \xE2\x80\xA2 enter select \xE2\x80\xA2 esc cancel";
    case TUI_MODAL_CONFIRM: return "y confirm \xE2\x80\xA2 n cancel";
    default: return "any key to dismiss";
    }
}

/* Bracket-free modal panel: full-width accent title bar, wrapped body,
 * input row, and an action-hint line. The backdrop dims the rest. */
/* Approval prompt text: " allow <tool>?  <args>   — y allow • n deny". */
static char *approval_prompt_text_alloc(const TuiApp *app)
{
    const char *title = tui_modal_title(app->modal);
    const char *body = tui_modal_body(app->modal);
    if (!title) title = "";
    if (!body) body = "";
    static const char hint[] = "   \xE2\x80\x94 y allow \xE2\x80\xA2 n deny";
    size_t n = strlen(title) + strlen(body) + strlen(hint) + 16;
    char *out = malloc(n);
    if (!out) return NULL;
    int k = snprintf(out, n, " allow %s?  %s%s", title, body, hint);
    if (k < 0 || (size_t)k >= n)
    {
        free(out);
        return NULL;
    }
    return out;
}

/* Wrapped height of the approval prompt bar at the current width; 0 when
 * no approval bar is up. Allocation failure falls back to one line so a
 * modal still renders. */
static int approval_bar_lines(const TuiApp *app, unsigned cols)
{
    if (!app->modal || tui_modal_kind(app->modal) != TUI_MODAL_APPROVAL)
        return 0;
    char *text = approval_prompt_text_alloc(app);
    if (!text) return 1;
    size_t width = cols >= 2 ? (size_t)cols - 1 : (size_t)cols;
    size_t lines = tui_chat_wrap(text, width, NULL, 0);
    free(text);
    return lines < 1 ? 1 : (int)lines;
}

/* Approval prompt bar: accent-background lines just above the input box —
 * no backdrop, no screen takeover. Fits on one line when the terminal is
 * wide enough, wraps across multiple lines otherwise so the full request
 * stays readable. */
static void render_prompt_bar(TuiApp *app)
{
    if (!app->prompt_plane || !app->modal) return;
    struct ncplane *p = app->prompt_plane;
    unsigned rows, cols;
    ncplane_dim_yx(p, &rows, &cols);
    ncplane_erase(p);
    plane_color(app, p, TUI_ROLE_STATUS_FG);

    char *text = approval_prompt_text_alloc(app);
    if (!text) return;
    size_t width = cols >= 2 ? (size_t)cols - 1 : (size_t)cols;
    size_t blines = tui_chat_wrap(text, width, NULL, 0);
    size_t *starts = malloc((blines + 1) * sizeof(size_t));
    if (starts)
    {
        (void)tui_chat_wrap(text, width, starts, blines + 1);
        starts[blines] = strlen(text);
        size_t shown = blines < rows ? blines : rows;
        for (size_t l = 0; l < shown; l++)
        {
            size_t s = starts[l];
            size_t e = starts[l + 1];
            if (e <= s) continue;
            (void)ncplane_cursor_move_yx(p, (int)l, 0);
            (void)ncplane_putnstr(p, e - s, text + s);
        }
        free(starts);
    }
    free(text);
}

static void render_modal(TuiApp *app)
{
    if (!app->modal)
    {
        /* Every close path funnels through here: planes left behind after
         * the modal struct is gone would sit on top of the z-buffer
         * forever, freezing the previous dialog on screen. */
        if (app->modal_plane)
        {
            ncplane_destroy(app->modal_plane);
            app->modal_plane = NULL;
        }
        if (app->backdrop_plane)
        {
            ncplane_destroy(app->backdrop_plane);
            app->backdrop_plane = NULL;
        }
        return;
    }
    /* Approvals render as the bottom prompt bar, not a full-screen modal */
    if (tui_modal_kind(app->modal) == TUI_MODAL_APPROVAL)
        return;
    unsigned rows, cols;
    notcurses_term_dim_yx(app->nc, &rows, &cols);
    int want = tui_modal_kind(app->modal) == TUI_MODAL_PICKER
                   ? tui_modal_picker_item_count(app->modal) + 4 : 8;
    int mh = want;
    if (mh < 8) mh = 8;
    if (mh > 20) mh = 20; /* picker lists cap the window */
    if ((int)rows < mh + 2) mh = (int)rows - 2;
    int my = ((int)rows - mh) / 2;

    /* Full-screen dim layer so content behind the modal is obscured */
    if (!app->backdrop_plane)
    {
        app->backdrop_plane = make_plane(app, rows, cols, 0, 0);
        if (app->backdrop_plane)
        {
            uint32_t bg = tui_theme_color(app->theme, TUI_ROLE_BACKDROP);
            (void)ncplane_set_bg_rgb8(app->backdrop_plane,
                                      (bg >> 16) & 0xff, (bg >> 8) & 0xff, bg & 0xff);
            (void)ncplane_set_base(app->backdrop_plane, " ", 0, 0);
            (void)ncplane_erase(app->backdrop_plane);
        }
    }
    if (!app->modal_plane)
        app->modal_plane = make_plane(app, (unsigned)mh, cols, my, 0);
    struct ncplane *p = app->modal_plane;
    ncplane_erase(p);

    /* Title bar: accent background, dark text. In transparent mode the
     * modal blends with the terminal: accent text, no background. */
    if (app->transparent)
    {
        uint32_t accent = tui_theme_color(app->theme, TUI_ROLE_ACCENT);
        (void)ncplane_set_fg_rgb8(p, (accent >> 16) & 0xff,
                                  (accent >> 8) & 0xff, accent & 0xff);
    }
    else
    {
        plane_color(app, p, TUI_ROLE_STATUS_FG);
    }
    (void)ncplane_set_styles(p, NCSTYLE_BOLD);
    const char *title = tui_modal_title(app->modal);
    size_t tn = strlen(title);
    if (tn > (size_t)cols - 2) tn = (size_t)cols - 2;
    (void)ncplane_putnstr(p, tn, title);
    (void)ncplane_set_styles(p, 0);

    /* Picker list: type-to-filter rows with a highlighted cursor. */
    if (tui_modal_kind(app->modal) == TUI_MODAL_PICKER)
    {
        int list_rows = mh - 3;
        if (list_rows < 1) list_rows = 1;
        tui_modal_picker_set_window(app->modal, list_rows);
        int top = tui_modal_picker_top(app->modal);
        int visible = tui_modal_picker_visible_count(app->modal);
        int cursor = tui_modal_picker_cursor(app->modal);
        if (visible == 0)
        {
            plane_color(app, p, TUI_ROLE_BORDER);
            (void)ncplane_cursor_move_yx(p, 1, 2);
            (void)ncplane_putstr(p, "(no matches)");
        }
        else
        {
            for (int l = 0; l < list_rows && top + l < visible; l++)
            {
                int row = top + l;
                if (row == cursor)
                {
                    uint32_t bg = tui_theme_color(app->theme, TUI_ROLE_STATUS_BG);
                    (void)ncplane_set_bg_rgb8(p, (bg >> 16) & 0xff,
                                              (bg >> 8) & 0xff, bg & 0xff);
                    uint32_t fg = tui_theme_color(app->theme, TUI_ROLE_STATUS_FG);
                    (void)ncplane_set_fg_rgb8(p, (fg >> 16) & 0xff,
                                              (fg >> 8) & 0xff, fg & 0xff);
                    (void)ncplane_set_styles(p, NCSTYLE_BOLD);
                }
                else
                {
                    plane_color(app, p, TUI_ROLE_BASE_FG);
                }
                const char *item = tui_modal_picker_visible_at(app->modal, row);
                if (item)
                {
                    size_t ilen = strlen(item);
                    if (ilen > (size_t)cols - 4) ilen = (size_t)cols - 4;
                    (void)ncplane_cursor_move_yx(p, 1 + l, 2);
                    (void)ncplane_putnstr(p, ilen, item);
                }
            }
        }
        /* Filter line (the picker reuses the input row for type-to-filter) */
        plane_color(app, p, TUI_ROLE_BASE_FG);
        (void)ncplane_cursor_move_yx(p, mh - 2, 2);
        (void)ncplane_putstr(p, "> ");
        const char *in = tui_modal_text(app->modal);
        size_t max = (size_t)cols - 4;
        size_t inlen = strlen(in);
        if (inlen > max) inlen = max;
        (void)ncplane_putnstr(p, inlen, in);

        plane_color(app, p, TUI_ROLE_BORDER);
        (void)ncplane_cursor_move_yx(p, mh - 1, 2);
        const char *phint = modal_hint(TUI_MODAL_PICKER);
        size_t phn = strlen(phint);
        if (phn > (size_t)cols - 4) phn = (size_t)cols - 4;
        (void)ncplane_putnstr(p, phn, phint);
        return;
    }

    /* Body: wrapped across the middle rows */
    plane_color(app, p, TUI_ROLE_BASE_FG);
    const char *body = tui_modal_body(app->modal);
    if (body)
    {
        int body_rows = mh - 3;
        if (body_rows < 1) body_rows = 1;
        size_t bw = cols >= 4 ? (size_t)cols - 4 : (size_t)cols;
        size_t blines = tui_chat_wrap(body, bw, NULL, 0);
        size_t *starts = malloc((blines + 1) * sizeof(size_t));
        if (starts)
        {
            (void)tui_chat_wrap(body, bw, starts, blines + 1);
            starts[blines] = strlen(body);
            size_t shown = blines < (size_t)body_rows ? blines : (size_t)body_rows;
            for (size_t l = 0; l < shown; l++)
            {
                size_t s = starts[l];
                size_t e = starts[l + 1];
                if (e <= s) continue;
                (void)ncplane_cursor_move_yx(p, 1 + (int)l, 2);
                (void)ncplane_putnstr(p, e - s, body + s);
            }
            if (blines > shown)
            {
                (void)ncplane_cursor_move_yx(p, 1 + (int)shown - 1, 2);
                (void)ncplane_putstr(p, "\xE2\x80\xA6"); /* … */
            }
            free(starts);
        }
    }

    /* Input row (password / ask_user) */
    if (tui_modal_needs_input(tui_modal_kind(app->modal)))
    {
        (void)ncplane_cursor_move_yx(p, mh - 2, 2);
        (void)ncplane_putstr(p, "> ");
        const char *in = tui_modal_text(app->modal);
        char masked[128];
        size_t max = (size_t)cols - 4;
        if (tui_modal_kind(app->modal) == TUI_MODAL_PASSWORD)
        {
            tui_modal_mask(in, masked, sizeof(masked));
            size_t mn = strlen(masked);
            if (mn > max) mn = max;
            (void)ncplane_putnstr(p, mn, masked);
        }
        else
        {
            size_t inlen = strlen(in);
            if (inlen > max) inlen = max;
            (void)ncplane_putnstr(p, inlen, in);
        }
    }

    /* Hint line */
    plane_color(app, p, TUI_ROLE_BORDER);
    (void)ncplane_cursor_move_yx(p, mh - 1, 2);
    const char *hint = modal_hint(tui_modal_kind(app->modal));
    size_t hn = strlen(hint);
    if (hn > (size_t)cols - 4) hn = (size_t)cols - 4;
    (void)ncplane_putnstr(p, hn, hint);
}

static void render_all(TuiApp *app)
{
    render_status(app);
    render_tools(app);
    render_chat(app);
    render_input(app);
    render_footer(app);
    render_prompt_bar(app);
    render_modal(app);
    (void)notcurses_render(app->nc);
}

/* ---- event dispatch ---- */

static void set_status_msg(TuiApp *app, const char *msg)
{
    free(app->status_msg);
    app->status_msg = msg ? str_dup(msg) : NULL;
}

static void handle_event(TuiApp *app, TuiEvent *ev)
{
    switch (ev->type)
    {
    case TUI_EV_CHUNK:
    case TUI_EV_THINK:
        if (tui_chat_block_count(app->chat) == 0 ||
            tui_chat_block_kind(app->chat, tui_chat_block_count(app->chat) - 1)
                != (ev->type == TUI_EV_THINK ? TUI_BLOCK_THINK : TUI_BLOCK_ASSISTANT))
        {
            (void)tui_chat_begin_stream(app->chat,
                                        ev->type == TUI_EV_THINK ? TUI_BLOCK_THINK : TUI_BLOCK_ASSISTANT);
        }
        (void)tui_chat_stream_append(app->chat, ev->text);
        break;
    case TUI_EV_TOOL_START:
        /* Open a pending tool block: empty content, spinner animation,
         * filled with the result when the tool ends. The block header
         * shows the call arguments compacted to one line, and the same
         * summary drives the live-activity strip. */
        {
            char *args = ev->extra
                             ? tool_args_compact_named(ev->text, ev->extra,
                                                       TOOL_ARGS_MAX)
                             : NULL;
            (void)tui_chat_begin_tool(app->chat, ev->text, args);
            set_status_msg(app, ev->text ? ev->text : "");
            free(app->active_tool);
            free(app->active_tool_args);
            app->active_tool = ev->text ? str_dup(ev->text) : NULL;
            app->active_tool_args = args; /* owned by the app from here */
        }
        break;
    case TUI_EV_TOOL_END:
        (void)tui_chat_tool_finish_named(app->chat, ev->text, ev->extra);
        /* clear the "busy" status hint and the activity strip */
        if (app->status_msg && ev->text &&
            strcmp(app->status_msg, ev->text) == 0)
            set_status_msg(app, NULL);
        free(app->active_tool);
        app->active_tool = NULL;
        free(app->active_tool_args);
        app->active_tool_args = NULL;
        break;
    case TUI_EV_TITLE:
        /* Session titles are internal bookkeeping: they belong in the
         * status bar, never in the message stream. */
        if (ev->text)
        {
            free(app->title);
            app->title = str_dup(ev->text);
        }
        break;
    case TUI_EV_PROVIDER:
        if (ev->text)
        {
            free(app->provider);
            app->provider = str_dup(ev->text);
        }
        break;
    case TUI_EV_MODEL:
        if (ev->text)
        {
            free(app->model);
            app->model = str_dup(ev->text);
        }
        break;
    case TUI_EV_MODELS:
        /* The worker joined the fetched model names with '\n' in extra;
         * split them into the picker's list (NULL/empty = no models). */
        for (int i = 0; i < app->pending_models_count; i++)
            free(app->pending_models[i]);
        free(app->pending_models);
        app->pending_models = NULL;
        app->pending_models_count = 0;
        if (ev->extra && ev->extra[0])
        {
            int cap = 1;
            const char *p = ev->extra;
            while (*p)
            {
                if (*p == '\n') cap++;
                p++;
            }
            app->pending_models = calloc((size_t)cap, sizeof(char *));
            if (app->pending_models)
            {
                int n = 0;
                const char *start = ev->extra;
                const char *q = ev->extra;
                while (1)
                {
                    if (*q == '\n' || *q == '\0')
                    {
                        size_t len = (size_t)(q - start);
                        if (len > 0)
                        {
                            app->pending_models[n] = strndup(start, len);
                            if (app->pending_models[n]) n++;
                        }
                        if (*q == '\0') break;
                        start = q + 1;
                    }
                    q++;
                }
                app->pending_models_count = n;
            }
        }
        /* A pending model picker (provider just switched) opens once the
         * list arrives, unless the user opened another dialog meanwhile. */
        if (app->picker_context == PICKER_MODEL && app->modal == NULL)
            open_model_picker(app);
        break;
    case TUI_EV_FORK:
        /* The worker forked at user/assistant message position ev->text
         * (1-based): drop every chat block from that message onwards so
         * the edited/regenerated reply streams into a clean tail. */
        {
            long n = ev->text ? strtol(ev->text, NULL, 10) : 0;
            if (n > 0)
            {
                long target = -1;
                long seen = 0;
                size_t count = tui_chat_block_count(app->chat);
                for (size_t i = 0; i < count; i++)
                {
                    TuiBlockKind k = tui_chat_block_kind(app->chat, i);
                    if (k == TUI_BLOCK_USER || k == TUI_BLOCK_ASSISTANT)
                    {
                        seen++;
                        if (seen == n)
                        {
                            target = (long)i;
                            break;
                        }
                    }
                }
                if (target >= 0)
                    (void)tui_chat_truncate_after(app->chat, (size_t)target);
                else
                {
                    tui_chat_destroy(app->chat);
                    app->chat = tui_chat_create();
                    app->chat_top = 0;
                }
            }
            /* Edit mode carries the replacement text; regenerate appends
             * nothing (the new reply streams in next). */
            if (ev->extra && ev->extra[0])
                (void)tui_chat_begin_user(app->chat, ev->extra);
            app->auto_scroll = 1;
        }
        break;
    case TUI_EV_HISTORY:
        rebuild_chat(app, ev->extra);
        break;
    case TUI_EV_SESSION:
        if (ev->text)
        {
            free(app->session_id);
            app->session_id = str_dup(ev->text);
        }
        break;
    case TUI_EV_STATUS:
        set_status_msg(app, ev->text);
        break;
    case TUI_EV_RUN_DONE:
        tui_chat_end_stream(app->chat);
        if (ev->extra && (!ev->text || ev->text[0] == '\0'))
            (void)tui_chat_append_error(app->chat, ev->extra);
        break;
    case TUI_EV_ASK_USER:
    case TUI_EV_APPROVAL:
        /* Round-trip ownership: the worker waits on the event's condvar
         * and frees it after the answer, so handle_event must NOT free.
         * If the modal cannot open (another dialog is up, e.g.
         * confirm-quit after Ctrl-C answered an ask_user), answer NULL
         * to unblock the worker — a dropped event would leave it waiting
         * forever and hang the quit path in pthread_join. */
        if (app->modal)
        {
            (void)tui_event_answer(ev, NULL, 0);
            return; /* worker frees the event */
        }
        {
            TuiModalKind kind = ev->type == TUI_EV_ASK_USER
                                    ? TUI_MODAL_ASK_USER : TUI_MODAL_APPROVAL;
            /* Approval: title = tool name, body = the arguments it would
             * run — rendered as the bottom prompt bar. The raw JSON is
             * compacted to one line so the bar stays readable. */
            const char *title = ev->type == TUI_EV_ASK_USER
                                    ? "ask_user" : ev->text;
            const char *body = ev->type == TUI_EV_ASK_USER
                                   ? ev->text : ev->extra;
            char *compacted = NULL;
            if (ev->type == TUI_EV_APPROVAL && ev->extra)
            {
                compacted = tool_args_compact(ev->extra, TOOL_ARGS_MAX);
                if (compacted) body = compacted;
            }
            app->modal = tui_modal_open(kind, title, body, ev);
            free(compacted);
            if (!app->modal)
                (void)tui_event_answer(ev, NULL, 0);
            else if (kind == TUI_MODAL_APPROVAL)
                app->term_rows = 0; /* grow the prompt bar into the layout */
        }
        return; /* never free round-trip events here */
    default:
        break;
    }
    tui_event_free(ev);
}

static void drain_events(TuiApp *app)
{
    TuiEvent *ev;
    while ((ev = tui_events_pop(app->ctx.evs)) != NULL)
        handle_event(app, ev);
    tui_events_drain_wake(app->ctx.evs);
}

/* ---- slash commands ---- */

static void chat_notice(TuiApp *app, const char *text)
{
    (void)tui_chat_append_error(app->chat, text);
}

/* ---- Ctrl-P provider/model picker ---- */

static void free_pending_models(TuiApp *app)
{
    for (int i = 0; i < app->pending_models_count; i++)
        free(app->pending_models[i]);
    free(app->pending_models);
    app->pending_models = NULL;
    app->pending_models_count = 0;
}

/* Join N strings with '\n' into a heap buffer ("" when count is 0). */
static char *join_newline(const char *const *items, int count)
{
    if (!items || count <= 0) return str_dup("");
    size_t total = 1U;
    for (int i = 0; i < count; i++)
        total += strlen(items[i]) + 1U;
    char *out = malloc(total);
    if (!out) return NULL;
    size_t pos = 0;
    for (int i = 0; i < count; i++)
    {
        pos += (size_t)snprintf(out + pos, total - pos, "%s\n", items[i]);
    }
    return out;
}

/* Session ids are timestamp-random tokens without spaces; menu picker
 * items render "id  title" and the leading token is the id. */
static char *session_id_from_item(const char *item)
{
    if (!item) return NULL;
    const char *sp = strchr(item, ' ');
    return strndup(item, sp ? (size_t)(sp - item) : strlen(item));
}

static void open_provider_picker(TuiApp *app)
{
    int count = 0;
    const char *const *names = provider_names_available(&count);
    if (count == 0)
    {
        chat_notice(app, "No providers available.");
        return;
    }
    /* A stale model list from a previous provider must not leak into the
     * next model picker. */
    free_pending_models(app);
    char *body = join_newline(names, count);
    if (!body)
    {
        chat_notice(app, "Out of memory opening provider picker.");
        return;
    }
    app->picker_context = PICKER_PROVIDER;
    app->modal = tui_modal_open(TUI_MODAL_PICKER, "Select provider", body, NULL);
    free(body);
    if (!app->modal)
    {
        app->picker_context = PICKER_NONE;
        chat_notice(app, "Out of memory opening provider picker.");
    }
}

static void open_model_picker(TuiApp *app)
{
    if (app->pending_models_count == 0)
    {
        chat_notice(app, "No models available; use /model <name>.");
        app->picker_context = PICKER_NONE;
        return;
    }
    char *body = join_newline((const char *const *)app->pending_models,
                              app->pending_models_count);
    if (!body)
    {
        chat_notice(app, "Out of memory opening model picker.");
        app->picker_context = PICKER_NONE;
        return;
    }
    free_pending_models(app);
    app->modal = tui_modal_open(TUI_MODAL_PICKER, "Select model", body, NULL);
    free(body);
    if (!app->modal)
    {
        app->picker_context = PICKER_NONE;
        chat_notice(app, "Out of memory opening model picker.");
    }
}

/* ---- main menu (Ctrl-M) ---- */

static void open_menu(TuiApp *app)
{
    static const char *items =
        "Provider / model\n"
        "Reasoning effort\n"
        "Change password\n"
        "Delete session\n"
        "Switch session\n"
        "Theme\n"
        "Export session\n"
        "Rename session";
    app->picker_context = PICKER_MENU;
    app->modal = tui_modal_open(TUI_MODAL_PICKER, "Menu", items, NULL);
    if (!app->modal)
    {
        app->picker_context = PICKER_NONE;
        chat_notice(app, "Out of memory opening menu.");
    }
}

static void open_effort_picker(TuiApp *app)
{
    const char *const *opts = provider_effort_options(app->provider);
    if (!opts)
    {
        chat_notice(app, "Current provider has no effort levels.");
        return;
    }
    int n = 0;
    while (opts[n]) n++;
    /* Collect levels + "default" into a single newline-joined string. */
    size_t total = 1U;
    for (int i = 0; i < n; i++) total += strlen(opts[i]) + 1U;
    total += strlen("default") + 1U;
    char *body = malloc(total);
    if (!body)
    {
        chat_notice(app, "Out of memory opening effort picker.");
        return;
    }
    size_t pos = 0;
    for (int i = 0; i < n; i++)
        pos += (size_t)snprintf(body + pos, total - pos, "%s\n", opts[i]);
    (void)snprintf(body + pos, total - pos, "default");
    app->picker_context = PICKER_EFFORT;
    app->modal = tui_modal_open(TUI_MODAL_PICKER, "Reasoning effort", body, NULL);
    free(body);
    if (!app->modal)
    {
        app->picker_context = PICKER_NONE;
        chat_notice(app, "Out of memory opening effort picker.");
    }
}

static void open_theme_picker(TuiApp *app)
{
    static const char *items = "dark\nlight\nhighcontrast\nnone";
    app->picker_context = PICKER_THEME;
    app->modal = tui_modal_open(TUI_MODAL_PICKER, "Theme", items, NULL);
    if (!app->modal)
    {
        app->picker_context = PICKER_NONE;
        chat_notice(app, "Out of memory opening theme picker.");
    }
}

static void open_session_picker(TuiApp *app, int context, const char *title)
{
    if (!app->ctx.sm)
    {
        chat_notice(app, "Session persistence disabled.");
        return;
    }
    SessionList *list = session_manager_list_sessions(app->ctx.sm);
    if (!list || list->count == 0)
    {
        if (list) session_list_free(list);
        chat_notice(app, "No sessions found.");
        return;
    }
    size_t total = 1U;
    for (int i = 0; i < list->count; i++)
        total += strlen(list->ids[i]) + strlen(list->titles[i]) + 3U;
    char *body = malloc(total);
    if (!body)
    {
        session_list_free(list);
        chat_notice(app, "Out of memory opening session picker.");
        return;
    }
    size_t pos = 0;
    for (int i = 0; i < list->count; i++)
        pos += (size_t)snprintf(body + pos, total - pos, "%s  %s\n",
                                list->ids[i], list->titles[i]);
    session_list_free(list);
    app->picker_context = context;
    app->modal = tui_modal_open(TUI_MODAL_PICKER, title, body, NULL);
    free(body);
    if (!app->modal)
    {
        app->picker_context = PICKER_NONE;
        chat_notice(app, "Out of memory opening session picker.");
    }
}

/* Pick a menu entry and open its next step (picker, input, or prompt). */
static void picker_menu_select(TuiApp *app, const char *sel)
{
    if (strcmp(sel, "Provider / model") == 0)
    {
        /* No key shortcut for the provider picker; the menu is its entry
         * point. A fetched model list opens the model picker next. */
        open_provider_picker(app);
    }
    else if (strcmp(sel, "Reasoning effort") == 0)
    {
        open_effort_picker(app);
    }
    else if (strcmp(sel, "Change password") == 0)
    {
        /* Blocking prompts: defer until the menu modal has closed. */
        app->picker_context = PICKER_NONE;
        app->menu_action = MENU_ACTION_CHANGE_PASSWORD;
    }
    else if (strcmp(sel, "Delete session") == 0)
    {
        open_session_picker(app, PICKER_SESSION_DELETE, "Delete session");
    }
    else if (strcmp(sel, "Switch session") == 0)
    {
        open_session_picker(app, PICKER_SESSION_LOAD, "Switch session");
    }
    else if (strcmp(sel, "Theme") == 0)
    {
        open_theme_picker(app);
    }
    else if (strcmp(sel, "Export session") == 0)
    {
        open_session_picker(app, PICKER_SESSION_EXPORT, "Export session");
    }
    else if (strcmp(sel, "Rename session") == 0)
    {
        open_session_picker(app, PICKER_SESSION_RENAME, "Rename session");
    }
    else
    {
        app->picker_context = PICKER_NONE;
    }
}

static void clear_menu_session(TuiApp *app)
{
    free(app->menu_session_id);
    app->menu_session_id = NULL;
}

/* Commit the picker selection. Returns 1 when a replacement modal was
 * opened (the caller must not close app->modal), 0 otherwise. */
static int picker_commit(TuiApp *app)
{
    const char *sel = tui_modal_picker_selected(app->modal);
    if (!sel)
    {
        app->picker_context = PICKER_NONE;
        clear_menu_session(app);
        app->input_action = INPUT_NONE;
        app->confirm_action = CONFIRM_NONE;
        return 0;
    }
    switch (app->picker_context)
    {
    case PICKER_PROVIDER:
        (void)tui_worker_submit(app->worker, "provider", sel);
        (void)tui_worker_submit(app->worker, "model-list", NULL);
        app->picker_context = PICKER_MODEL;
        set_status_msg(app, "Fetching models \xE2\x80\xA6");
        return 0;
    case PICKER_MODEL:
        (void)tui_worker_submit(app->worker, "model", sel);
        app->picker_context = PICKER_NONE;
        return 0;
    case PICKER_MENU:
        picker_menu_select(app, sel);
        return app->picker_context != PICKER_NONE;
    case PICKER_EFFORT:
        (void)tui_worker_submit(app->worker, "effort", sel);
        app->picker_context = PICKER_NONE;
        return 0;
    case PICKER_THEME:
        slash_theme(app, sel);
        app->picker_context = PICKER_NONE;
        return 0;
    case PICKER_SESSION_LOAD:
        clear_menu_session(app);
        app->menu_session_id = session_id_from_item(sel);
        if (app->menu_session_id)
        {
            (void)tui_worker_submit(app->worker, "load", app->menu_session_id);
            clear_menu_session(app);
        }
        app->picker_context = PICKER_NONE;
        return 0;
    case PICKER_SESSION_DELETE:
        clear_menu_session(app);
        app->menu_session_id = session_id_from_item(sel);
        app->picker_context = PICKER_NONE;
        app->confirm_action = CONFIRM_DELETE;
        app->modal = tui_modal_open(TUI_MODAL_CONFIRM, "Delete session",
                                    "Delete this session permanently?",
                                    NULL);
        if (!app->modal)
        {
            app->confirm_action = CONFIRM_NONE;
            clear_menu_session(app);
        }
        return 1;
    case PICKER_SESSION_EXPORT:
        clear_menu_session(app);
        app->menu_session_id = session_id_from_item(sel);
        app->picker_context = PICKER_NONE;
        app->input_action = INPUT_EXPORT_PATH;
        app->modal = tui_modal_open(TUI_MODAL_ASK_USER,
                                    "Export path",
                                    "Path (empty = <session id>.json)",
                                    NULL);
        if (!app->modal)
        {
            app->input_action = INPUT_NONE;
            clear_menu_session(app);
        }
        return 1;
    case PICKER_SESSION_RENAME:
        clear_menu_session(app);
        app->menu_session_id = session_id_from_item(sel);
        app->picker_context = PICKER_NONE;
        app->input_action = INPUT_RENAME_NAME;
        app->modal = tui_modal_open(TUI_MODAL_ASK_USER, "Rename session",
                                    "New name:", NULL);
        if (!app->modal)
        {
            app->input_action = INPUT_NONE;
            clear_menu_session(app);
        }
        return 1;
    default:
        app->picker_context = PICKER_NONE;
        return 0;
    }
}

/* Commit the text collected by a menu-opened input modal. */
static void input_commit(TuiApp *app)
{
    const char *text = tui_modal_text(app->modal);
    int action = app->input_action;
    app->input_action = INPUT_NONE;
    if (!text || !text[0])
    {
        clear_menu_session(app);
        return; /* empty input cancels */
    }
    if (action == INPUT_EXPORT_PATH && app->menu_session_id)
    {
        char *arg = NULL;
        if (asprintf(&arg, "%s\x1f%s", app->menu_session_id, text) >= 0)
        {
            (void)tui_worker_submit(app->worker, "export", arg);
            free(arg);
        }
    }
    else if (action == INPUT_RENAME_NAME && app->menu_session_id)
    {
        char *arg = NULL;
        if (asprintf(&arg, "%s\x1f%s", app->menu_session_id, text) >= 0)
        {
            (void)tui_worker_submit(app->worker, "rename", arg);
            free(arg);
        }
    }
    clear_menu_session(app);
}

/* Commit a menu-opened confirm modal's decision. */
static void confirm_commit(TuiApp *app)
{
    int yes = app->modal->selection;
    int action = app->confirm_action;
    app->confirm_action = CONFIRM_NONE;
    if (action == CONFIRM_DELETE && yes && app->menu_session_id)
        (void)tui_worker_submit(app->worker, "delete", app->menu_session_id);
    clear_menu_session(app);
}

static void slash_save(TuiApp *app, const char *name)
{
    if (!app->ctx.sm || !app->worker)
    {
        chat_notice(app, "Session persistence disabled.");
        return;
    }
    /* Use the status-bar session id: owned by the app and valid across
     * /new, unlike the agent's (which the worker may have replaced). */
    if (!app->session_id)
    {
        chat_notice(app, "No active session to save.");
        return;
    }
    Session *s = session_manager_load_session_alloc(app->ctx.sm, app->session_id);
    if (!s)
    {
        chat_notice(app, "No active session to save.");
        return;
    }
    /* Duplicate before freeing the old title: on allocation failure the
     * session keeps its current title instead of losing it to NULL. */
    char *title_copy = str_dup(name);
    if (!title_copy)
    {
        session_free(s);
        chat_notice(app, "Failed to save session.");
        return;
    }
    free(s->title);
    s->title = title_copy;
    if (session_manager_save_session(app->ctx.sm, s) == 0)
        chat_notice(app, "Session saved.");
    else
        chat_notice(app, "Failed to save session.");
    session_free(s);
}

/* Case-insensitive substring (ASCII folding; portable, no strcasestr). */
static int ci_contains(const char *haystack, const char *needle)
{
    if (!needle || !needle[0]) return 1;
    if (!haystack) return 0;
    size_t hn = strlen(haystack);
    size_t nn = strlen(needle);
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++)
    {
        int match = 1;
        for (size_t j = 0; j < nn; j++)
        {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j]))
            {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

static void slash_sessions(TuiApp *app, const char *term)
{
    if (!app->ctx.sm)
    {
        chat_notice(app, "Session persistence disabled.");
        return;
    }
    SessionList *list = session_manager_list_sessions(app->ctx.sm);
    if (!list)
    {
        chat_notice(app, "No sessions found.");
        return;
    }
    int shown = 0;
    for (int i = 0; i < list->count; i++)
    {
        /* Case-insensitive filter over title and id (feature: session
         * search). Empty term lists everything. */
        if (term && term[0])
        {
            if (!ci_contains(list->titles[i], term) &&
                !ci_contains(list->ids[i], term))
                continue;
        }
        char line[512];
        snprintf(line, sizeof(line), "  %s | %s | %s",
                 list->ids[i], list->titles[i], list->created_ats[i]);
        (void)tui_chat_append_error(app->chat, line);
        shown++;
    }
    if (shown == 0)
    {
        char line[160];
        snprintf(line, sizeof(line), "No sessions match: %s", term);
        chat_notice(app, line);
    }
    session_list_free(list);
}

static void slash_help(TuiApp *app)
{
    static const char *help =
        "/exit /quit  quit   /new  reset conversation\n"
        "/save <n> save session   /load <id> load session\n"
        "/delete <id> delete   /rename <id> <name> rename\n"
        "/model <m> switch model  /sessions [term] list/search\n"
        "/provider <p> switch provider  /effort <v> reasoning effort\n"
        "Ctrl-P menu   /copy [n] copy message\n"
        "/edit <#|text> edit message   /regen <#> regenerate reply\n"
        "/branch [id] list/switch branches\n"
        "/theme <dark|light|highcontrast|none>\n"
        "/export <id> [path]   /change-password\n"
        "/openai-login /openai-logout  /lock /unlock\n"
        "/undo /redo change tracker   /clear clear pane  /help this text";
    (void)tui_chat_begin_stream(app->chat, TUI_BLOCK_TOOL);
    (void)tui_chat_stream_append(app->chat, help);
    tui_chat_end_stream(app->chat);
}

/* ---- password change (two prompt modals, then a worker job) ---- */

static void slash_change_password(TuiApp *app)
{
    if (tui_worker_busy(app->worker))
    {
        chat_notice(app, "Busy - wait for the current run.");
        return;
    }
    if (!app->ctx.sm)
    {
        chat_notice(app, "Session persistence disabled.");
        return;
    }
    char *pw1 = tui_app_prompt_password(app, "New database password: ");
    if (!pw1) return;
    char *pw2 = tui_app_prompt_password(app, "Confirm new database password: ");
    if (!pw2)
    {
        memset(pw1, 0, strlen(pw1));
        free(pw1);
        return;
    }
    if (strcmp(pw1, pw2) != 0)
    {
        chat_notice(app, "Passwords do not match.");
        memset(pw1, 0, strlen(pw1));
        free(pw1);
        memset(pw2, 0, strlen(pw2));
        free(pw2);
        return;
    }
    (void)tui_worker_submit(app->worker, "change-password", pw1);
    memset(pw1, 0, strlen(pw1));
    free(pw1);
    memset(pw2, 0, strlen(pw2));
    free(pw2);
}

/* ---- runtime theme switch ---- */

static void slash_theme(TuiApp *app, const char *style)
{
    if (!style || !style[0])
    {
        chat_notice(app, "Usage: /theme dark|light|highcontrast|none");
        return;
    }
    TuiTheme *nt = tui_theme_create(style, app->ctx.density, app->ctx.accent);
    if (!nt)
    {
        chat_notice(app, "Theme switch failed (out of memory).");
        return;
    }
    tui_theme_free(app->theme);
    app->theme = nt;
    /* The next render_all() recolors every plane from the new theme. */
    char msg[96];
    snprintf(msg, sizeof(msg), "Theme: %s", style);
    chat_notice(app, msg);
}

/* ---- clipboard copy via OSC 52 ---- */

static void osc52_copy(const char *text)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = text ? strlen(text) : 0;
    size_t cap = ((n + 2U) / 3U) * 4U + 1U;
    char *b64 = malloc(cap);
    if (!b64) return;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3)
    {
        unsigned v = (unsigned)(unsigned char)text[i] << 16;
        if (i + 1U < n) v |= (unsigned)(unsigned char)text[i + 1U] << 8;
        if (i + 2U < n) v |= (unsigned)(unsigned char)text[i + 2U];
        b64[o++] = tbl[(v >> 18) & 0x3f];
        b64[o++] = tbl[(v >> 12) & 0x3f];
        b64[o++] = (i + 1U < n) ? tbl[(v >> 6) & 0x3f] : '=';
        b64[o++] = (i + 2U < n) ? tbl[v & 0x3f] : '=';
    }
    b64[o] = '\0';
    char *seq = NULL;
    if (asprintf(&seq, "\x1b]52;c;%s\x07", b64) >= 0)
    {
        /* OSC 52: set the terminal clipboard to base64 text; harmless in
         * terminals that ignore it. */
        ssize_t written = write(STDOUT_FILENO, seq, strlen(seq));
        (void)written;
        free(seq);
    }
    free(b64);
}

static void slash_copy(TuiApp *app, const char *arg)
{
    long want = -1;
    if (arg && arg[0])
    {
        char *end = NULL;
        want = strtol(arg, &end, 10);
        if (!end || *end != '\0' || want <= 0)
        {
            chat_notice(app, "Usage: /copy [message number]");
            return;
        }
    }
    size_t count = tui_chat_block_count(app->chat);
    long target = -1;
    long seen = 0;
    for (size_t i = 0; i < count; i++)
    {
        TuiBlockKind k = tui_chat_block_kind(app->chat, i);
        if (k == TUI_BLOCK_USER || k == TUI_BLOCK_ASSISTANT)
        {
            seen++;
            if (want > 0 && seen == want)
            {
                target = (long)i;
                break;
            }
            target = (long)i; /* default: keep the last message */
        }
    }
    if (target < 0)
    {
        chat_notice(app, "Nothing to copy.");
        return;
    }
    const char *text = tui_chat_block_text(app->chat, (size_t)target);
    osc52_copy(text);
    chat_notice(app, "Copied message to clipboard.");
}

/* ---- edit / regenerate / branches ---- */

/* Block index of the nth user/assistant message (1-based), or -1. */
static long message_block_at(TuiApp *app, long n)
{
    size_t count = tui_chat_block_count(app->chat);
    long seen = 0;
    for (size_t i = 0; i < count; i++)
    {
        TuiBlockKind k = tui_chat_block_kind(app->chat, i);
        if (k == TUI_BLOCK_USER || k == TUI_BLOCK_ASSISTANT)
        {
            seen++;
            if (seen == n) return (long)i;
        }
    }
    return -1;
}

static void slash_edit(TuiApp *app, const char *rest)
{
    char *end = NULL;
    long n = strtol(rest, &end, 10);
    if (!end || *end != ' ' || n < 1)
    {
        chat_notice(app, "Usage: /edit <message #> <new text>");
        return;
    }
    const char *text = end + 1;
    if (!text[0])
    {
        chat_notice(app, "Empty edit text.");
        return;
    }
    long blk = message_block_at(app, n);
    if (blk < 0)
    {
        chat_notice(app, "Message not found.");
        return;
    }
    if (tui_chat_block_kind(app->chat, (size_t)blk) != TUI_BLOCK_USER)
    {
        chat_notice(app, "Only user messages can be edited.");
        return;
    }
    char *arg = NULL;
    if (asprintf(&arg, "%ld\x1f%s", n, text) >= 0)
    {
        (void)tui_worker_submit(app->worker, "edit", arg);
        free(arg);
    }
}

static void slash_regen(TuiApp *app, const char *rest)
{
    char *end = NULL;
    long n = strtol(rest, &end, 10);
    if (!end || *end != '\0' || n < 1)
    {
        chat_notice(app, "Usage: /regen <message #>");
        return;
    }
    long blk = message_block_at(app, n);
    if (blk < 0)
    {
        chat_notice(app, "Message not found.");
        return;
    }
    if (tui_chat_block_kind(app->chat, (size_t)blk) != TUI_BLOCK_ASSISTANT)
    {
        chat_notice(app, "Only assistant replies can be regenerated.");
        return;
    }
    char arg[32];
    snprintf(arg, sizeof(arg), "%ld", n);
    (void)tui_worker_submit(app->worker, "regen", arg);
}

/* Rebuild the scrollback from a messages_to_json_array() payload (branch
 * switch). Best-effort: allocation failures just skip a block. */
static void rebuild_chat(TuiApp *app, const char *json)
{
    if (!json) return;
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsArray(root))
    {
        if (root) cJSON_Delete(root);
        return;
    }
    TuiChat *nc = tui_chat_create();
    if (!nc)
    {
        cJSON_Delete(root);
        return;
    }
    int count = cJSON_GetArraySize(root);
    for (int i = 0; i < count; i++)
    {
        cJSON *m = cJSON_GetArrayItem(root, i);
        if (!m) continue;
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(m, "role"));
        const char *content =
            cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
        const char *thinking =
            cJSON_GetStringValue(cJSON_GetObjectItem(m, "thinking"));
        if (thinking && thinking[0])
        {
            if (tui_chat_begin_stream(nc, TUI_BLOCK_THINK) == 0)
            {
                (void)tui_chat_stream_append(nc, thinking);
                tui_chat_end_stream(nc);
            }
        }
        if (role && strcmp(role, "user") == 0)
        {
            (void)tui_chat_begin_user(nc, content ? content : "");
        }
        else if (role && strcmp(role, "assistant") == 0)
        {
            if (tui_chat_begin_stream(nc, TUI_BLOCK_ASSISTANT) == 0)
            {
                (void)tui_chat_stream_append(nc, content ? content : "");
                tui_chat_end_stream(nc);
            }
        }
        cJSON *tc = cJSON_GetObjectItem(m, "tool_calls");
        if (tc && cJSON_IsArray(tc))
        {
            int tn = cJSON_GetArraySize(tc);
            for (int j = 0; j < tn; j++)
            {
                cJSON *call = cJSON_GetArrayItem(tc, j);
                cJSON *fn = call ? cJSON_GetObjectItem(call, "function") : NULL;
                const char *name = fn
                    ? cJSON_GetStringValue(cJSON_GetObjectItem(fn, "name"))
                    : NULL;
                const char *result =
                    cJSON_GetStringValue(cJSON_GetObjectItem(call, "result_content"));
                const char *err =
                    cJSON_GetStringValue(cJSON_GetObjectItem(call, "result_error"));
                if (err && err[0]) result = err;
                (void)tui_chat_append_tool(nc, name ? name : "tool",
                                           result ? result : "");
            }
        }
    }
    tui_chat_destroy(app->chat);
    app->chat = nc;
    app->chat_top = 0;
    app->auto_scroll = 1;
    cJSON_Delete(root);
}

static void handle_command(TuiApp *app, const char *cmd)
{
    if (strcmp(cmd, "/exit") == 0 || strcmp(cmd, "/quit") == 0)
    {
        app->quit = 1;
        return;
    }
    if (strcmp(cmd, "/help") == 0) { slash_help(app); return; }
    if (strcmp(cmd, "/clear") == 0)
    {
        tui_chat_destroy(app->chat);
        app->chat = tui_chat_create();
        app->chat_top = 0;
        return;
    }
    if (strcmp(cmd, "/new") == 0)
    {
        (void)tui_worker_submit(app->worker, "new", NULL);
        return;
    }
    if (strcmp(cmd, "/undo") == 0)
    {
        int rc = ct_undo(app->ctx.ct);
        if (rc < 0) chat_notice(app, "Nothing to undo.");
        else chat_notice(app, "Undone.");
        return;
    }
    if (strcmp(cmd, "/redo") == 0)
    {
        int rc = ct_redo(app->ctx.ct);
        if (rc < 0) chat_notice(app, "Nothing to redo.");
        else chat_notice(app, "Redone.");
        return;
    }
    if (strncmp(cmd, "/sessions", 9) == 0)
    {
        /* Optional filter term after "/sessions " searches titles/ids. */
        const char *term = cmd[9] == ' ' ? cmd + 10 : NULL;
        slash_sessions(app, term);
        return;
    }
    if (strncmp(cmd, "/save ", 6) == 0) { slash_save(app, cmd + 6); return; }
    if (strncmp(cmd, "/load ", 6) == 0)
    {
        (void)tui_worker_submit(app->worker, "load", cmd + 6);
        return;
    }
    if (strncmp(cmd, "/model ", 7) == 0)
    {
        (void)tui_worker_submit(app->worker, "model", cmd + 7);
        return;
    }
    if (strncmp(cmd, "/provider ", 10) == 0)
    {
        (void)tui_worker_submit(app->worker, "provider", cmd + 10);
        return;
    }
    if (strncmp(cmd, "/effort ", 8) == 0)
    {
        (void)tui_worker_submit(app->worker, "effort", cmd + 8);
        return;
    }
    if (strncmp(cmd, "/delete ", 8) == 0)
    {
        (void)tui_worker_submit(app->worker, "delete", cmd + 8);
        return;
    }
    if (strncmp(cmd, "/rename ", 8) == 0)
    {
        /* "/rename <id> <name>": name may contain spaces, so split on the
         * first space after the id. */
        const char *rest = cmd + 8;
        const char *sp = strchr(rest, ' ');
        if (sp && sp[1])
        {
            char *arg = NULL;
            if (asprintf(&arg, "%.*s\x1f%s", (int)(sp - rest), rest, sp + 1) >= 0)
            {
                (void)tui_worker_submit(app->worker, "rename", arg);
                free(arg);
            }
        }
        else
        {
            chat_notice(app, "Usage: /rename <id> <name>");
        }
        return;
    }
    if (strncmp(cmd, "/export", 7) == 0)
    {
        const char *rest = cmd[7] == ' ' ? cmd + 8 : NULL;
        if (!rest || !rest[0])
        {
            chat_notice(app, "Usage: /export <id> [path]");
            return;
        }
        const char *sp = strchr(rest, ' ');
        char *arg = NULL;
        int made = sp
            ? asprintf(&arg, "%.*s\x1f%s", (int)(sp - rest), rest, sp + 1)
            : asprintf(&arg, "%s\x1f", rest);
        if (made >= 0)
        {
            (void)tui_worker_submit(app->worker, "export", arg);
            free(arg);
        }
        return;
    }
    if (strcmp(cmd, "/change-password") == 0)
    {
        slash_change_password(app);
        return;
    }
    if (strcmp(cmd, "/openai-login") == 0)
    {
        (void)tui_worker_submit(app->worker, "openai-login", NULL);
        return;
    }
    if (strcmp(cmd, "/openai-logout") == 0)
    {
        (void)tui_worker_submit(app->worker, "openai-logout", NULL);
        return;
    }
    if (strcmp(cmd, "/menu") == 0)
    {
        open_menu(app);
        return;
    }
    if (strcmp(cmd, "/lock") == 0)
    {
        (void)tui_worker_submit(app->worker, "lock", NULL);
        return;
    }
    if (strcmp(cmd, "/unlock") == 0)
    {
        (void)tui_worker_submit(app->worker, "unlock", NULL);
        return;
    }
    if (strncmp(cmd, "/theme ", 7) == 0)
    {
        slash_theme(app, cmd + 7);
        return;
    }
    if (strncmp(cmd, "/copy", 5) == 0)
    {
        slash_copy(app, cmd[5] == ' ' ? cmd + 6 : NULL);
        return;
    }
    if (strncmp(cmd, "/edit ", 6) == 0)
    {
        slash_edit(app, cmd + 6);
        return;
    }
    if (strncmp(cmd, "/regen ", 7) == 0)
    {
        slash_regen(app, cmd + 7);
        return;
    }
    if (strncmp(cmd, "/branch", 7) == 0)
    {
        const char *bid = cmd[7] == ' ' ? cmd + 8 : NULL;
        if (bid && bid[0])
            (void)tui_worker_submit(app->worker, "branch", bid);
        else
            (void)tui_worker_submit(app->worker, "branch-info", NULL);
        return;
    }
    chat_notice(app, "Unknown command. Try /help.");
}

/* ---- input handling ---- */

static void submit_line(TuiApp *app)
{
    char *line = tui_input_submit(app->input);
    if (!line) return;
    if (line[0] == '/')
    {
        handle_command(app, line);
    }
    else if (line[0] != '\0')
    {
        (void)tui_chat_begin_user(app->chat, line);
        int rc = tui_worker_submit(app->worker, "run", line);
        if (rc == -2) chat_notice(app, "Busy - wait for the current run.");
        else if (rc == -1) chat_notice(app, "Could not queue the message.");
    }
    free(line);
}

/* Map a click (SGR coordinates: 1-based, terminal-root) to the chat pane
 * and toggle the collapse state of long tool blocks. */
static void handle_click(TuiApp *app, const ncinput *ni)
{
    if (app->modal) return; /* modals are keyboard-only in v1 */

    unsigned rows, cols;
    ncplane_dim_yx(app->chat_plane, &rows, &cols);
    int y = ni->y - 1; /* SGR is 1-based */
    int x = ni->x - 1;
    if (y < 2 || y >= 2 + (int)rows) return; /* outside the chat pane */
    if (x < 0 || x >= (int)cols) return;
    size_t cw = cols >= 6 ? (size_t)cols - 3 : (size_t)cols; /* rail + pad */

    size_t row = (size_t)(y - 2);
    size_t virtual_line = app->chat_top + row;

    size_t cursor = 0;
    for (size_t b = 0; b < tui_chat_block_count(app->chat); b++)
    {
        size_t blines = tui_chat_block_render_lines(app->chat, b, cw);
        if (virtual_line < cursor + blines)
        {
            if (tui_chat_block_kind(app->chat, b) == TUI_BLOCK_TOOL)
            {
                size_t full = 0;
                (void)tui_chat_block_line_starts(app->chat, b, cw, &full);
                if (full > TUI_CHAT_COLLAPSE_THRESHOLD)
                    (void)tui_chat_toggle_collapse(app->chat, b, cw);
            }
            return;
        }
        cursor += blines + 1; /* separator line after each block */
    }
}

/* Close the current modal, answering any attached round-trip event with
 * NULL so a waiting worker can never hang, and dropping the planes the
 * next render would otherwise redraw stale. */
static void close_modal(TuiApp *app, int answer_cancel)
{
    if (!app->modal) return;
    if (answer_cancel && app->modal->event)
        (void)tui_event_answer(app->modal->event, NULL, 0);
    int was_approval = tui_modal_kind(app->modal) == TUI_MODAL_APPROVAL;
    tui_modal_close(app->modal);
    app->modal = NULL;
    if (app->modal_plane)
    {
        ncplane_destroy(app->modal_plane);
        app->modal_plane = NULL;
    }
    if (app->backdrop_plane)
    {
        ncplane_destroy(app->backdrop_plane);
        app->backdrop_plane = NULL;
    }
    if (was_approval)
        app->term_rows = 0; /* drop the prompt bar from the layout */
}

/* Text keys arrive as utf8 in ni->utf8; control/special keys carry NCKEY_*
 * ids. Returns the codepoint-style key for modals, or -1 when the event
 * was consumed here (editing keys). */
static void handle_key(TuiApp *app, const ncinput *ni)
{
    uint32_t id = ni->id;

    if (app->modal)
    {
        /* Ctrl-C always quits: cancel the pending modal (answering its
         * event so the worker never hangs) and offer the quit dialog. */
        if (key_is_ctrl(ni, 'C'))
        {
            close_modal(app, 1);
            app->modal = tui_modal_open(TUI_MODAL_CONFIRM_QUIT,
                                        "Quit?", "Press y to quit, any other key to cancel.", NULL);
            return;
        }
        int key = -1;
        if (id == NCKEY_ENTER) key = 10;
        else if (id == NCKEY_ESC) key = 27;
        else if (id == NCKEY_BACKSPACE) key = 127;
        else if (tui_modal_kind(app->modal) == TUI_MODAL_PICKER)
        {
            /* The picker also navigates with arrows / PgUp-PgDn and
             * Ctrl-P/Ctrl-N, so route those into the modal dispatcher. */
            if (id == NCKEY_UP) key = TUI_PICKER_KEY_UP;
            else if (id == NCKEY_DOWN) key = TUI_PICKER_KEY_DOWN;
            else if (id == NCKEY_PGUP) key = TUI_PICKER_KEY_PGUP;
            else if (id == NCKEY_PGDOWN) key = TUI_PICKER_KEY_PGDOWN;
            else if (key_is_ctrl(ni, 'P')) key = TUI_PICKER_KEY_UP;
            else if (key_is_ctrl(ni, 'N')) key = TUI_PICKER_KEY_DOWN;
            else if (id < 0x110000u && id >= 32u) key = ncinput_cp(ni);
        }
        else if (id < 0x110000u && id >= 32u) key = ncinput_cp(ni);
        /* key == -1 means "nothing mapped"; the negative picker sentinels
         * are valid keys and must reach the dispatcher. */
        if (key != -1)
        {
            TuiModalAction action = tui_modal_dispatch_key(app->modal, key);
            if (action == TUI_MODAL_ACTION_QUIT)
            {
                close_modal(app, 1);
                app->quit = 1;
            }
            else if (action == TUI_MODAL_ACTION_ANSWERED)
            {
                int replaced = 0;
                if (tui_modal_kind(app->modal) == TUI_MODAL_PICKER)
                    replaced = picker_commit(app);
                else if (tui_modal_kind(app->modal) == TUI_MODAL_ASK_USER &&
                         app->modal->event == NULL &&
                         app->input_action != INPUT_NONE)
                    input_commit(app);
                else if (tui_modal_kind(app->modal) == TUI_MODAL_CONFIRM &&
                         app->confirm_action != CONFIRM_NONE)
                    confirm_commit(app);
                if (!replaced)
                    close_modal(app, 0); /* dispatch already answered the event */
            }
        }
        return;
    }

    if (key_is_ctrl(ni, 'M') || key_is_ctrl(ni, 'P')) /* main menu */
    {
        open_menu(app);
        return;
    }

    if (id == NCKEY_ESC)
    {
        if (tui_worker_busy(app->worker)) tui_worker_cancel(app->worker);
        return;
    }
    if (id == NCKEY_ENTER)
    {
        /* Shift+Enter inserts a newline (multiline input); plain Enter
         * submits the whole buffer. */
        if (ncinput_shift_p(ni))
        {
            tui_input_reset_history_walk(app->input);
            (void)tui_input_insert(app->input, "\n");
            return;
        }
        submit_line(app);
        return;
    }
    if (id == NCKEY_BACKSPACE)
    {
        tui_input_reset_history_walk(app->input); /* editing ends the walk */
        (void)tui_input_backspace(app->input);
        return;
    }
    if (id == NCKEY_DEL)
    {
        tui_input_reset_history_walk(app->input);
        (void)tui_input_delete(app->input);
        return;
    }
    if (key_is_ctrl(ni, 'W')) /* Ctrl-W: delete the word before the cursor */
    {
        tui_input_reset_history_walk(app->input);
        (void)tui_input_delete_word(app->input);
        return;
    }
    if (id == NCKEY_LEFT) { tui_input_move(app->input, -1); return; }
    if (id == NCKEY_RIGHT) { tui_input_move(app->input, 1); return; }
    if (id == NCKEY_HOME) { tui_input_home(app->input); return; }
    if (id == NCKEY_END) { tui_input_end(app->input); return; }
    if (id == NCKEY_UP) { (void)tui_input_history_back(app->input); return; }
    if (id == NCKEY_DOWN) { (void)tui_input_history_forward(app->input); return; }
    if (id == NCKEY_PGUP || id == NCKEY_SCROLL_UP)
    {
        /* Scroll toward earlier content: the viewport top moves down
         * toward the beginning of the conversation. */
        app->auto_scroll = 0;
        app->chat_top = app->chat_top > 5 ? app->chat_top - 5 : 0;
        return;
    }
    if (id == NCKEY_PGDOWN || id == NCKEY_SCROLL_DOWN)
    {
        unsigned rows, cols;
        ncplane_dim_yx(app->chat_plane, &rows, &cols);
        /* Clamp with the same wrap width the renderer uses */
        size_t cw = cols >= 6 ? (size_t)cols - 3 : (size_t)cols;
        size_t max_top = tui_chat_view_clamp(app->chat, cw, rows, SIZE_MAX);
        app->chat_top += 5;
        if (app->chat_top >= max_top)
        {
            app->chat_top = max_top;
            app->auto_scroll = 1; /* at the bottom: pin to new content */
        }
        else
        {
            app->auto_scroll = 0;
        }
        return;
    }
    if (key_is_ctrl(ni, 'C')) /* Ctrl-C */
    {
        app->modal = tui_modal_open(TUI_MODAL_CONFIRM_QUIT,
                                    "Quit?", "Press y to quit, any other key to cancel.", NULL);
        return;
    }
    if (id == NCKEY_BUTTON1)
    {
        handle_click(app, ni);
        return;
    }
    if (key_is_ctrl(ni, 'L')) /* Ctrl-L */
    {
        tui_chat_destroy(app->chat);
        app->chat = tui_chat_create();
        app->chat_top = 0;
        return;
    }
    /* Any printable key types, including modifier combos like AltGr
     * (reported as ctrl+alt) that produce characters on some layouts;
     * the ctrl bindings above already consumed Ctrl-C/W/L. Pure text
     * events (IME/dead keys report key 0 with the text in eff_text)
     * insert through the effective-text path. */
    if ((id >= 32u && id < 0x110000u) ||
        (ni->eff_text[0] != 0 && ni->eff_text[0] != ni->id))
    {
        char buf[24];
        size_t n = ncinput_text(ni, buf, sizeof(buf));
        tui_input_reset_history_walk(app->input); /* typing ends the walk */
        (void)tui_input_insert(app->input, n > 0 ? buf : "");
    }
}

/* ---- main loop ---- */

static int app_loop(TuiApp *app)
{
    struct pollfd fds[2];
    fds[0].fd = tui_events_wake_fd(app->ctx.evs);
    fds[1].fd = tui_events_wake_fd(app->ctx.jobs);
    fds[0].events = POLLIN;
    fds[1].events = POLLIN;

    while (!app->quit)
    {
        check_signals(app);
        if (app->quit) break;
        /* Run deferred blocking menu actions once their picker has
         * closed (the password prompts cannot run inside modal
         * dispatch). */
        if (app->menu_action != MENU_ACTION_NONE && app->modal == NULL)
        {
            int act = app->menu_action;
            app->menu_action = MENU_ACTION_NONE;
            if (act == MENU_ACTION_CHANGE_PASSWORD)
                slash_change_password(app);
        }
        if (layout_planes(app) != 0)
        {
            /* Terminal too small: wait for a resize instead of failing */
            ncinput ni;
            uint32_t rc;
            while ((rc = notcurses_get_nblock(app->nc, &ni)) != 0 && rc != UINT32_MAX)
            {
                if (ni.evtype == NCTYPE_RELEASE) continue; /* kitty prot. */
                if (ni.id == NCKEY_RESIZE || key_is_ctrl(&ni, 'C')) break;
            }
            if (rc == UINT32_MAX) { app->quit = 1; break; } /* EOF: terminal gone */
            check_signals(app);
            if (app->quit) break;
            continue;
        }
        /* 50ms tick drives the spinner; keys are drained below */
        int prc = poll(fds, 2, 50);
        if (prc > 0)
        {
            drain_events(app);
            /* drain the job ring's wake bytes (UI never pops it) */
            tui_events_drain_wake(app->ctx.jobs);
        }
        ncinput ni;
        uint32_t rc;
        while ((rc = notcurses_get_nblock(app->nc, &ni)) != 0 && rc != UINT32_MAX)
        {
            /* Terminals with the kitty keyboard protocol report key
             * release events; skipping them prevents double-input. */
            if (ni.evtype == NCTYPE_RELEASE) continue;
            if (ni.id == NCKEY_RESIZE)
            {
                app->term_rows = 0; /* force relayout */
                continue;
            }
            handle_key(app, &ni);
        }
        if (rc == UINT32_MAX) { app->quit = 1; break; } /* EOF: terminal gone */
        if (app->auto_scroll)
        {
            unsigned rows, cols;
            ncplane_dim_yx(app->chat_plane, &rows, &cols);
            size_t cw = cols >= 6 ? (size_t)cols - 3 : (size_t)cols;
            size_t total = tui_chat_total_lines(app->chat, cw);
            if (total > (size_t)rows) app->chat_top = total - (size_t)rows;
        }
        app->frame++;
        render_all(app);
    }
    return 0;
}

/* ---- public API ---- */

TuiApp *tui_app_create(const TuiAppCtx *ctx)
{
    if (!ctx) return NULL;
    TuiApp *app = calloc(1, sizeof(TuiApp));
    if (!app) return NULL;
    app->ctx = *ctx;
    app->model = ctx->model ? str_dup(ctx->model) : NULL;
    app->provider = ctx->provider ? str_dup(ctx->provider) : NULL;
    app->session_id = ctx->session_id ? str_dup(ctx->session_id) : NULL;
    app->log_path = ctx->log_path ? str_dup(ctx->log_path) : default_log_path();

    app->theme = tui_theme_create(ctx->style, ctx->density, ctx->accent);
    app->transparent = ctx->transparent;
    app->chat = tui_chat_create();
    app->input = tui_input_create(32);
    if (!app->theme || !app->chat || !app->input)
        goto fail;

    /* freopen(NULL, ...) is undefined: fail instead when neither the
     * caller nor HOME provided a log path. */
    if (!app->log_path)
    {
        log_error("tui: no stderr log path (HOME unset?)", NULL);
        goto fail;
    }
    if (redirect_stderr(app->log_path) != 0)
        goto fail;

    struct notcurses_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.flags = NCOPTION_SUPPRESS_BANNERS;
    app->nc = notcurses_init(&opts, NULL);
    if (!app->nc)
    {
        log_error("tui: notcurses init failed", NULL);
        goto fail;
    }
    /* Wheel events are button events (BUTTON4/5) — needed for the
     * PgUp/PgDn-equivalent mouse scrolling in the chat pane. */
    (void)notcurses_mice_enable(app->nc, NCMICE_BUTTON_EVENT);

    /* SIGINT/SIGTERM set a flag instead of killing the process: the loop
     * notices within one tick and tears down through notcurses_stop, so
     * the terminal is always restored. */
    g_sig_quit = 0;
    if (install_signal_handlers(app->saved_sigs, 2) != 0)
        log_error("tui: signal handlers not installed", NULL);

    app->auto_scroll = 1;
    return app;

fail:
    tui_app_destroy(app);
    return NULL;
}

char *tui_app_prompt_password(TuiApp *app, const char *prompt)
{
    if (!app) return NULL;
    app->modal = tui_modal_open(TUI_MODAL_PASSWORD, "Session password", prompt, NULL);
    if (!app->modal) return NULL;

    char *result = NULL;
    int done = 0;
    while (!done && !app->quit)
    {
        check_signals(app);
        if (app->quit) break;
        if (layout_planes(app) != 0)
        {
            /* Terminal too small: skip rendering (planes are NULL) and
             * wait for a resize before re-offering the prompt. */
            if (wait_for_terminal(app) != 0) break;
            continue;
        }
        ncinput ni;
        uint32_t rc;
        while ((rc = notcurses_get_nblock(app->nc, &ni)) != 0 && rc != UINT32_MAX)
        {
            if (ni.evtype == NCTYPE_RELEASE) continue; /* kitty prot. */
            int key = -1;
            if (ni.id == NCKEY_ENTER) key = 10;
            else if (ni.id == NCKEY_ESC) key = 27;
            else if (ni.id == NCKEY_BACKSPACE) key = 127;
            else if (ni.id < 0x110000u && ni.id >= 32u) key = ncinput_cp(&ni);
            if (key < 0) continue;
            TuiModalAction action = tui_modal_dispatch_key(app->modal, key);
            if (action == TUI_MODAL_ACTION_ANSWERED)
            {
                /* empty input = cancel, matching the getpass flow */
                result = tui_input_len(app->modal->input) > 0
                             ? str_dup(tui_input_text(app->modal->input))
                             : NULL;
                tui_modal_close(app->modal);
                app->modal = NULL;
                done = 1;
                break;
            }
        }
        if (rc == UINT32_MAX) { app->quit = 1; break; } /* EOF: terminal gone */
        render_all(app);
    }
    return result;
}

/* Notice modal: any key dismisses; returns 0. Used for unlock errors. */
int tui_app_notice(TuiApp *app, const char *title, const char *body)
{
    if (!app) return -1;
    app->modal = tui_modal_open(TUI_MODAL_NOTICE, title, body, NULL);
    if (!app->modal) return -1;

    int done = 0;
    while (!done && !app->quit)
    {
        check_signals(app);
        if (app->quit) break;
        if (layout_planes(app) != 0)
        {
            /* Terminal too small: skip rendering (planes are NULL) and
             * wait for a resize before re-offering the notice. */
            if (wait_for_terminal(app) != 0) break;
            continue;
        }
        ncinput ni;
        uint32_t rc;
        while ((rc = notcurses_get_nblock(app->nc, &ni)) != 0 && rc != UINT32_MAX)
        {
            if (ni.evtype == NCTYPE_RELEASE) continue; /* kitty prot. */
            if (ni.id == NCKEY_RESIZE) continue;
            (void)tui_modal_dispatch_key(app->modal, 10);
            tui_modal_close(app->modal);
            app->modal = NULL;
            done = 1;
            break;
        }
        if (rc == UINT32_MAX) { app->quit = 1; break; } /* EOF: terminal gone */
        render_all(app);
    }
    return 0;
}

int tui_app_run(TuiApp *app, SessionManager *sm)
{
    if (!app) return -1;
    app->ctx.sm = sm;
    if (sm && app->ctx.agent)
    {
        registry_set_session_manager(sm);
        agent_set_session_manager(app->ctx.agent, sm);
    }

    /* Fresh screens need a hint: the chat pane is empty and the tool
     * strip is blank until the first run. */
    (void)tui_chat_begin_stream(app->chat, TUI_BLOCK_THINK);
    (void)tui_chat_stream_append(app->chat,
        "Welcome to Echo AI. Type a message to chat, /help for commands.");
    tui_chat_end_stream(app->chat);

    TuiWorkerCtx wctx;
    memset(&wctx, 0, sizeof(wctx));
    wctx.agent = app->ctx.agent;
    wctx.evs = app->ctx.evs;
    wctx.jobs = app->ctx.jobs;
    wctx.sm = sm;
    wctx.conf = app->ctx.conf;
    wctx.oauth = app->ctx.oauth;
    wctx.safety = app->ctx.safety;
    wctx.agent_factory = app->ctx.agent_factory;
    wctx.agent_factory_userdata = app->ctx.agent_factory_userdata;
    app->worker = tui_worker_create(&wctx);
    if (!app->worker)
    {
        log_error("tui: worker start failed", NULL);
        return -1;
    }

    int rc = app_loop(app);

    /* A run may still be winding down after cancel (in-flight LLM calls
     * finish first): say so instead of freezing on a stale frame. */
    close_modal(app, 1);
    set_status_msg(app, "Quitting \xE2\x80\xA6");
    /* Farewell in the chat pane so the final frame says goodbye before
     * the terminal is restored (best-effort: allocation failures just
     * skip the block). */
    (void)tui_chat_begin_stream(app->chat, TUI_BLOCK_ASSISTANT);
    (void)tui_chat_stream_append(app->chat, "Goodbye! See you next time.");
    (void)tui_chat_end_stream(app->chat);
    render_all(app);

    tui_worker_destroy(app->worker);
    app->worker = NULL;
    return rc;
}

void tui_app_destroy(TuiApp *app)
{
    if (!app) return;
    restore_signal_handlers(app->saved_sigs, 2);
    /* Never leave a round-trip event unanswered: a worker might still be
     * waiting on it (e.g. quit during an in-flight ask_user). */
    if (app->modal && app->modal->event)
        (void)tui_event_answer(app->modal->event, NULL, 0);
    tui_modal_close(app->modal);
    app->modal = NULL;
    destroy_planes(app);
    if (app->nc) (void)notcurses_stop(app->nc);
    app->nc = NULL;
    tui_input_destroy(app->input);
    tui_chat_destroy(app->chat);
    tui_theme_free(app->theme);
    free(app->active_tool);
    free(app->active_tool_args);
    free(app->title);
    free(app->status_msg);
    free(app->session_id);
    free(app->model);
    free(app->provider);
    free(app->menu_session_id);
    for (int i = 0; i < app->pending_models_count; i++)
        free(app->pending_models[i]);
    free(app->pending_models);
    free(app->log_path);
    free(app);
}
