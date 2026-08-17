/*
 * tui.c - the TUI application shell: notcurses lifecycle, layout, the
 * poll-based main loop (two event wake fds + notcurses keys), event
 * dispatch into the chat/tool/status models, and key handling. All agent
 * mutation goes through the worker; this file never touches the agent
 * except through tui_worker_* calls and the read-only context fields.
 * Rendering lives in tui_render.c; commands/pickers in tui_cmd.c.
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
#include <time.h>
#include <wchar.h>
#include <wctype.h>

#include <notcurses/notcurses.h>

#include "tui_internal.h"
#include "markdown.h"
#include "tool_args.h"

/* Cap for the compacted tool-arguments text shown on header lines. */
#define TOOL_ARGS_MAX 200

/* Forward decls local to this file. */
static void keymap_bind_def(TuiKeymap *km, const char *name, const char *cat,
                            const char *desc, const char *keys);
static void keymap_register_defaults(TuiKeymap *km);
static void keymap_apply_config(TuiApp *app);
static TuiKeyStroke stroke_from_ncinput(const ncinput *ni);

/* ---- signal handling: graceful exit on SIGINT/SIGTERM ---- */

static volatile sig_atomic_t g_sig_quit = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_sig_quit = 1; /* async-signal-safe: a flag is all we touch here */
}

int install_signal_handlers(struct sigaction *saved, size_t n_saved)
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

void restore_signal_handlers(const struct sigaction *saved,
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
        app->sidebar_plane, app->modal_plane, app->backdrop_plane
    };
    for (size_t i = 0; i < sizeof(planes) / sizeof(planes[0]); i++)
    {
        if (planes[i]) ncplane_destroy(planes[i]);
    }
    app->status_plane = app->chat_plane = app->tools_plane = NULL;
    app->input_plane = app->footer_plane = app->prompt_plane = NULL;
    app->sidebar_plane = app->modal_plane = app->backdrop_plane = NULL;
}

struct ncplane *make_plane(TuiApp *app, unsigned rows, unsigned cols,
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

/* Rebuild the plane stack when the terminal size changes — or when the
 * approval bar appears/disappears (callers invalidate term_rows).
 * approval_bar_lines is declared in tui_internal.h (tui_render.c). */
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
    /* chat shrinks by the approval bar height while it is up, and by the
     * sidebar width while the sidebar is shown */
    int chat_h = (int)rows - 3 - bar - (int)app->input_rows;
    if (chat_h < 1) chat_h = 1;
    unsigned chat_w = cols;
    unsigned side_w = 0;
    if (app->sidebar_visible && cols >= 40)
    {
        side_w = cols >= 80 ? 30 : cols / 3;
        if (side_w > 40) side_w = 40;
        chat_w = cols - side_w;
    }
    app->chat_plane = make_plane(app, (unsigned)chat_h, chat_w, 2, 0);
    if (side_w > 0)
        app->sidebar_plane = make_plane(app, (unsigned)chat_h, side_w, 2,
                                        (int)chat_w);
    else
        app->sidebar_plane = NULL;
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
void set_status_msg(TuiApp *app, const char *msg)
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
            if (app->models_path)
                (void)tui_model_store_record_recent(app->models_path,
                                                    ev->text, 20);
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

void drain_events(TuiApp *app)
{
    TuiEvent *ev;
    while ((ev = tui_events_pop(app->ctx.evs)) != NULL)
        handle_event(app, ev);
    tui_events_drain_wake(app->ctx.evs);
}

/* ---- slash commands ---- */

/* ---- keymap wiring ----
 * Convert notcurses input into a normalized TuiKeyStroke, register the
 * default command bindings, and dispatch matched commands through a
 * small name->action table. Everything here delegates to the existing
 * handlers; the keymap owns only which keys reach them. */

static TuiKeyStroke stroke_from_ncinput(const ncinput *ni)
{
    TuiKeyStroke s;
    memset(&s, 0, sizeof(s));
    uint32_t id = ni->id;

    /* NCKEY_TAB and NCKEY_ESC are plain ASCII values (0x09/0x1b), so
     * they must be mapped before the preterunicode range check. */
    if (id == NCKEY_TAB) s.id = TUI_KEYID_TAB;
    else if (id == NCKEY_ESC) s.id = TUI_KEYID_ESC;
    else if (id >= 0x110000u) /* notcurses preterunicode special keys */
    {
        switch (id)
        {
        case NCKEY_ENTER: s.id = TUI_KEYID_ENTER; break;
        case NCKEY_BACKSPACE: s.id = TUI_KEYID_BACKSPACE; break;
        case NCKEY_DEL: s.id = TUI_KEYID_DEL; break;
        case NCKEY_INS: s.id = TUI_KEYID_INSERT; break;
        case NCKEY_HOME: s.id = TUI_KEYID_HOME; break;
        case NCKEY_END: s.id = TUI_KEYID_END; break;
        case NCKEY_PGUP: s.id = TUI_KEYID_PGUP; break;
        case NCKEY_PGDOWN: s.id = TUI_KEYID_PGDOWN; break;
        case NCKEY_UP: s.id = TUI_KEYID_UP; break;
        case NCKEY_DOWN: s.id = TUI_KEYID_DOWN; break;
        case NCKEY_LEFT: s.id = TUI_KEYID_LEFT; break;
        case NCKEY_RIGHT: s.id = TUI_KEYID_RIGHT; break;
        case NCKEY_SCROLL_UP: s.id = TUI_KEYID_SCROLL_UP; break;
        case NCKEY_SCROLL_DOWN: s.id = TUI_KEYID_SCROLL_DOWN; break;
        default:
            if (id >= NCKEY_F01 && id <= NCKEY_F12)
                s.id = TUI_KEYID_F1 + (id - NCKEY_F01);
            else
                s.id = TUI_KEYID_NONE; /* unknown special: unmatchable */
            break;
        }
    }
    else
    {
        /* prefer the effective text when it differs (kitty protocol
         * reports layout-produced characters there) */
        if (ni->eff_text[0] != 0 && ni->eff_text[0] != ni->id)
            s.id = ni->eff_text[0];
        else
            s.id = id;
    }
    s.ctrl = (ni->modifiers & NCKEY_MOD_CTRL) != 0;
    s.shift = (ni->modifiers & NCKEY_MOD_SHIFT) != 0;
    s.alt = (ni->modifiers & NCKEY_MOD_ALT) != 0;
    s.meta = (ni->modifiers & NCKEY_MOD_META) != 0;
    s.super = (ni->modifiers & NCKEY_MOD_SUPER) != 0;
    tui_key_stroke_normalize(&s);
    return s;
}

static void keymap_bind_def(TuiKeymap *km, const char *name, const char *cat,
                            const char *desc, const char *keys)
{
    TuiKeyBinding b;
    memset(&b, 0, sizeof(b));
    snprintf(b.name, sizeof(b.name), "%s", name);
    snprintf(b.category, sizeof(b.category), "%s", cat);
    snprintf(b.desc, sizeof(b.desc), "%s", desc);
    snprintf(b.keys, sizeof(b.keys), "%s", keys);
    /* A default spec is authored by us and must parse; a failure is a
     * build bug, so log it rather than silently binding nothing. */
    if (tui_keymap_register(km, &b) != 0)
        log_error("tui: bad default keybind '%s'", name);
}

/* Register the full default command set. Keys mirror opencode's bindings
 * where the command exists; echo-ai's modal/worker semantics stay. */
static void keymap_register_defaults(TuiKeymap *km)
{
    keymap_bind_def(km, "app.quit_prompt", "App", "Ask before quitting", "ctrl+c,ctrl+d,leader+q");
    keymap_bind_def(km, "command.palette.show", "App", "Open the command palette", "ctrl+p");
    keymap_bind_def(km, "menu.open", "App", "Open the classic menu", "ctrl+m");
    keymap_bind_def(km, "run.cancel", "App", "Cancel the running reply", "esc");
    keymap_bind_def(km, "chat.clear", "Chat", "Clear the chat pane", "ctrl+l");
    keymap_bind_def(km, "chat.scroll_up", "Chat", "Scroll toward older messages", "pgup,scrollup");
    keymap_bind_def(km, "chat.scroll_down", "Chat", "Scroll toward newer messages", "pgdown,scrolldown");
    keymap_bind_def(km, "session.list", "Session", "List and switch sessions", "leader+l");
    keymap_bind_def(km, "session.new", "Session", "Start a new session", "leader+n");
    keymap_bind_def(km, "session.undo", "Session", "Undo the last file change", "leader+u");
    keymap_bind_def(km, "session.redo", "Session", "Redo the last undone change", "leader+r");
    keymap_bind_def(km, "session.timeline", "Session", "Jump to a user message", "leader+g");
    keymap_bind_def(km, "session.fork", "Session", "Re-answer from a user message", "leader+f");
    keymap_bind_def(km, "session.pin", "Session", "Pin or unpin the active session", "leader+p");
    keymap_bind_def(km, "session.sidebar.toggle", "Session", "Toggle the sidebar", "leader+b");
    keymap_bind_def(km, "prompt.editor", "Prompt", "Open the draft in $EDITOR", "leader+e");
    keymap_bind_def(km, "session.quick_switch.1", "Session", "Quick switch slot 1", "leader+1");
    keymap_bind_def(km, "session.quick_switch.2", "Session", "Quick switch slot 2", "leader+2");
    keymap_bind_def(km, "session.quick_switch.3", "Session", "Quick switch slot 3", "leader+3");
    keymap_bind_def(km, "session.quick_switch.4", "Session", "Quick switch slot 4", "leader+4");
    keymap_bind_def(km, "session.quick_switch.5", "Session", "Quick switch slot 5", "leader+5");
    keymap_bind_def(km, "session.quick_switch.6", "Session", "Quick switch slot 6", "leader+6");
    keymap_bind_def(km, "session.quick_switch.7", "Session", "Quick switch slot 7", "leader+7");
    keymap_bind_def(km, "session.quick_switch.8", "Session", "Quick switch slot 8", "leader+8");
    keymap_bind_def(km, "session.quick_switch.9", "Session", "Quick switch slot 9", "leader+9");
    keymap_bind_def(km, "model.list", "Model", "Pick provider or model", "leader+m");
    keymap_bind_def(km, "model.cycle_recent", "Model", "Next recent model", "f2");
    keymap_bind_def(km, "model.cycle_recent_reverse", "Model", "Previous recent model", "shift+f2");
    keymap_bind_def(km, "theme.list", "Theme", "Pick a theme", "leader+t");
    keymap_bind_def(km, "input.submit", "Input", "Submit the input", "enter");
    keymap_bind_def(km, "input.newline", "Input", "Insert a newline", "shift+enter");
    keymap_bind_def(km, "prompt.complete", "Input", "Complete a slash command", "tab");
    keymap_bind_def(km, "input.backspace", "Input", "Delete backward", "backspace");
    keymap_bind_def(km, "input.delete", "Input", "Delete forward", "del");
    keymap_bind_def(km, "input.delete_word", "Input", "Delete the word before the cursor", "ctrl+w");
    keymap_bind_def(km, "input.move.left", "Input", "Move the cursor left", "left");
    keymap_bind_def(km, "input.move.right", "Input", "Move the cursor right", "right");
    keymap_bind_def(km, "input.home", "Input", "Jump to the line start", "home");
    keymap_bind_def(km, "input.end", "Input", "Jump to the line end", "end");
    keymap_bind_def(km, "input.history_back", "Input", "Previous prompt", "up");
    keymap_bind_def(km, "input.history_forward", "Input", "Next prompt", "down");
}

/* Apply [tui] config overrides: tui.leader, tui.leader_timeout, and
 * tui.keymap_<command> = "<keys>|none" per binding. Unknown commands and
 * unparseable specs are logged and skipped (config convention). */
static void keymap_apply_config(TuiApp *app)
{
    const Conf *conf = app->ctx.conf;
    if (!conf) return;

    const char *leader = conf_get(conf, "tui.leader");
    if (leader && leader[0] && tui_keymap_set_leader(app->keymap, leader) != 0)
        log_error("tui: bad leader key '%s'", leader);

    const char *lt = conf_get(conf, "tui.leader_timeout");
    if (lt && lt[0])
    {
        char *end = NULL;
        long ms = strtol(lt, &end, 10);
        if (end && *end == '\0' && ms >= 1)
            tui_keymap_set_leader_timeout(app->keymap, (uint64_t)ms);
        else
            log_error("tui: bad leader_timeout '%s'", lt);
    }

    for (int i = 0; i < tui_keymap_count(app->keymap); i++)
    {
        const TuiKeyBinding *b = tui_keymap_at(app->keymap, i);
        char key[64];
        if (snprintf(key, sizeof(key), "tui.keymap_%s", b->name) >= (int)sizeof(key))
            continue;
        const char *val = conf_get(conf, key);
        if (!val || val[0] == '\0') continue;
        if (tui_keymap_bind(app->keymap, b->name, val) != 0)
            log_error("tui: bad keybind override %s='%s'", key, val);
    }
}


uint64_t mono_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ---- input handling ---- */

void submit_line(TuiApp *app)
{
    char *line = tui_input_submit(app->input);
    if (!line) return;
    if (line[0] == '!')
    {
        /* shell mode: run the text after '!' as a shell command */
        (void)tui_chat_begin_user(app->chat, line);
        int rc = tui_worker_submit(app->worker, "shell", line + 1);
        if (rc == -2) chat_notice(app, "Busy - wait for the current run.");
        else if (rc == -1) chat_notice(app, "Could not queue the command.");
        free(line);
        return;
    }
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
        else if (app->history_path)
            (void)tui_prompt_store_history_append(app->history_path, line,
                                                  NULL, 50);
    }
    free(line);
}

/* Map a click (SGR coordinates: 1-based, terminal-root) to the chat pane
 * and toggle the collapse state of long tool blocks. */
void handle_click(TuiApp *app, const ncinput *ni)
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
void close_modal(TuiApp *app, int answer_cancel)
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
        else if (tui_modal_kind(app->modal) == TUI_MODAL_APPROVAL)
        {
            /* the approval prompt navigates its three options with
             * left/right (routed as the picker's up/down sentinels) */
            if (id == NCKEY_LEFT) key = TUI_PICKER_KEY_UP;
            else if (id == NCKEY_RIGHT) key = TUI_PICKER_KEY_DOWN;
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
                else if (tui_modal_kind(app->modal) == TUI_MODAL_APPROVAL)
                    approval_commit(app); /* apply the "Always" rule */
                else if (tui_modal_kind(app->modal) == TUI_MODAL_CONFIRM &&
                         app->confirm_action != CONFIRM_NONE)
                    confirm_commit(app);
                if (!replaced)
                    close_modal(app, 0); /* dispatch already answered the event */
            }
        }
        return;
    }

    if (id == NCKEY_BUTTON1)
    {
        handle_click(app, ni);
        return;
    }

    /* Everything else goes through the keymap: matched commands dispatch
     * to the named action; unmatched keys fall through to text input. */
    TuiKeyStroke stroke = stroke_from_ncinput(ni);
    const char *name = NULL;
    uint64_t now = mono_now_ms();
    TuiKeymapResult res = tui_keymap_dispatch(app->keymap, &stroke, now, &name);
    if (res == TUI_KEYMAP_CMD)
    {
        command_dispatch(app, name, NULL);
        return;
    }
    if (res == TUI_KEYMAP_LEADER)
        return; /* leader armed (or a chord consumed); footer shows a hint */

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
                slash_change_password(app, NULL);
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
        (void)tui_keymap_leader_expired(app->keymap, mono_now_ms());
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
    app->keymap = tui_keymap_create();
    app->commands = tui_command_registry_create();
    if (!app->theme || !app->chat || !app->input || !app->keymap || !app->commands)
        goto fail;
    keymap_register_defaults(app->keymap);
    keymap_apply_config(app);
    commands_init(app);
    prompt_store_init(app);

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
    tui_keymap_destroy(app->keymap);
    tui_command_registry_destroy(app->commands);
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
    free(app->history_path);
    free(app->stash_path);
    free(app->pins_path);
    free(app->models_path);
    free(app);
}
