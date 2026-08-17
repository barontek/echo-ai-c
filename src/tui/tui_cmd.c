/*
 * tui_cmd.c - the TUI's command layer: slash-command handlers, the
 * command registry registration, command dispatch, the command palette,
 * the prompt stash, and the /status and /debug dialogs. Split out of
 * tui.c; this file drives the worker, the chat model, the input editor,
 * and the modal stack — never the agent directly.
 * Depends on: tui_internal.h, tui_prompt_store.h, factory.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include <notcurses/notcurses.h>

#include "tui_internal.h"
#include "tui_prompt_store.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"
#include "../tools/registry.h"
#include "../session/session.h"
#include "../llm/factory.h"
#include "../safety/safety.h"

/* Forward decls local to this file. */
static void cmd_editor(TuiApp *app, const char *args);
void cmd_model_cycle_recent(TuiApp *app, const char *args);
void cmd_model_cycle_recent_reverse(TuiApp *app, const char *args);
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
        snprintf(line, sizeof(line), "No sessions match: %s",
                 term ? term : "(none)");
        chat_notice(app, line);
    }
    session_list_free(list);
}

static void slash_help(TuiApp *app, const char *args)
{
    (void)args;
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
        "/undo /redo change tracker   /clear clear pane\n"
        "/keybinds show the keymap   /help this text";
    (void)tui_chat_begin_stream(app->chat, TUI_BLOCK_TOOL);
    (void)tui_chat_stream_append(app->chat, help);
    tui_chat_end_stream(app->chat);
}

/* Build a newline-joined keymap listing and open it as a read-only
 * picker (type-to-filter, Esc closes). Lines: "name  <keys>  (desc)". */
static void slash_keybinds(TuiApp *app, const char *args)
{
    (void)args;
    int n = tui_keymap_count(app->keymap);
    if (n <= 0)
    {
        chat_notice(app, "No keybindings registered.");
        return;
    }
    size_t total = 1U;
    for (int i = 0; i < n; i++)
    {
        const TuiKeyBinding *b = tui_keymap_at(app->keymap, i);
        total += strlen(b->name) + strlen(b->desc) + 120;
    }
    char *body = malloc(total);
    if (!body)
    {
        chat_notice(app, "Out of memory opening keybindings.");
        return;
    }
    size_t pos = 0;
    for (int i = 0; i < n; i++)
    {
        const TuiKeyBinding *b = tui_keymap_at(app->keymap, i);
        char line[260];
        snprintf(line, sizeof(line), "%s  %-12s (%s)%s", b->name,
                 b->enabled ? b->keys : "none",
                 b->desc, i + 1 < n ? "\n" : "");
        pos += (size_t)snprintf(body + pos, total - pos, "%s", line);
    }
    app->picker_context = PICKER_KEYBINDS;
    app->modal = tui_modal_open(TUI_MODAL_PICKER, "Keybindings", body, NULL);
    free(body);
    if (!app->modal)
    {
        app->picker_context = PICKER_NONE;
        chat_notice(app, "Out of memory opening keybindings.");
    }
}

/* ---- password change (two prompt modals, then a worker job) ---- */

void slash_change_password(TuiApp *app, const char *args)
{
    (void)args;
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

void slash_theme(TuiApp *app, const char *style)
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
long message_block_at(TuiApp *app, long n)
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
void rebuild_chat(TuiApp *app, const char *json)
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

/* Slash-command dispatch through the registry: parse "/name [args]",
 * resolve the name (primary slash name or alias), and invoke the
 * command's handler with the trailing text. */
void handle_command(TuiApp *app, const char *cmd)
{
    const char *p = cmd + 1; /* skip '/' */
    size_t nlen = 0;
    while (p[nlen] && p[nlen] != ' ') nlen++;
    if (nlen == 0)
    {
        chat_notice(app, "Unknown command. Try /help.");
        return;
    }
    char name[32];
    if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
    memcpy(name, p, nlen);
    name[nlen] = '\0';
    const char *args = p[nlen] == ' ' ? p + nlen + 1 : NULL;

    const TuiCommand *c = tui_command_find_slash(app->commands, name);
    if (!c || !c->fn)
    {
        chat_notice(app, "Unknown command. Try /help.");
        return;
    }
    c->fn(app, args);
}

/* ---- thin slash wrappers (the registry's fn is (TuiApp *, args)) ---- */

static void quit_immediate(TuiApp *app, const char *args)
{
    (void)args;
    app->quit = 1;
}
static void slash_load(TuiApp *app, const char *args)
{
    if (!args || !args[0])
    {
        chat_notice(app, "Usage: /load <session id>");
        return;
    }
    (void)tui_worker_submit(app->worker, "load", args);
}
static void slash_delete(TuiApp *app, const char *args)
{
    if (!args || !args[0])
    {
        chat_notice(app, "Usage: /delete <session id>");
        return;
    }
    (void)tui_worker_submit(app->worker, "delete", args);
}
static void slash_rename(TuiApp *app, const char *args)
{
    /* "/rename <id> <name>": name may contain spaces, so split on the
     * first space after the id. */
    if (!args || !args[0])
    {
        chat_notice(app, "Usage: /rename <id> <name>");
        return;
    }
    const char *sp = strchr(args, ' ');
    if (sp && sp[1])
    {
        char *arg = NULL;
        if (asprintf(&arg, "%.*s\x1f%s", (int)(sp - args), args, sp + 1) >= 0)
        {
            (void)tui_worker_submit(app->worker, "rename", arg);
            free(arg);
        }
    }
    else
    {
        chat_notice(app, "Usage: /rename <id> <name>");
    }
}
static void slash_export(TuiApp *app, const char *args)
{
    if (!args || !args[0])
    {
        chat_notice(app, "Usage: /export <id> [path]");
        return;
    }
    const char *sp = strchr(args, ' ');
    char *arg = NULL;
    int made = sp
        ? asprintf(&arg, "%.*s\x1f%s", (int)(sp - args), args, sp + 1)
        : asprintf(&arg, "%s\x1f", args);
    if (made >= 0)
    {
        (void)tui_worker_submit(app->worker, "export", arg);
        free(arg);
    }
}
static void slash_branch(TuiApp *app, const char *args)
{
    if (args && args[0])
        (void)tui_worker_submit(app->worker, "branch", args);
    else
        (void)tui_worker_submit(app->worker, "branch-info", NULL);
}
static void slash_model(TuiApp *app, const char *args)
{
    if (!args || !args[0])
    {
        chat_notice(app, "Usage: /model <name>");
        return;
    }
    (void)tui_worker_submit(app->worker, "model", args);
}
static void slash_provider(TuiApp *app, const char *args)
{
    if (!args || !args[0])
    {
        chat_notice(app, "Usage: /provider <name>");
        return;
    }
    (void)tui_worker_submit(app->worker, "provider", args);
}
static void slash_effort(TuiApp *app, const char *args)
{
    if (!args || !args[0])
    {
        chat_notice(app, "Usage: /effort <level>");
        return;
    }
    (void)tui_worker_submit(app->worker, "effort", args);
}
static void slash_lock(TuiApp *app, const char *args)
{
    (void)args;
    (void)tui_worker_submit(app->worker, "lock", NULL);
}
static void slash_unlock(TuiApp *app, const char *args)
{
    (void)args;
    (void)tui_worker_submit(app->worker, "unlock", NULL);
}
static void slash_openai_login(TuiApp *app, const char *args)
{
    (void)args;
    (void)tui_worker_submit(app->worker, "openai-login", NULL);
}
static void slash_openai_logout(TuiApp *app, const char *args)
{
    (void)args;
    (void)tui_worker_submit(app->worker, "openai-logout", NULL);
}

/* ---- command handlers ----
 * Every handler has the registry signature (TuiApp *, const char *args);
 * keymap/palette dispatches pass NULL args. The registry holds metadata
 * plus the handler; the keymap and slash commands both resolve through
 * it, so a command's keys, slash form, and palette entry are one object. */

static void cmd_open_menu(TuiApp *app, const char *args)
{
    (void)args;
    open_menu(app);
}
static void cmd_quit_prompt(TuiApp *app, const char *args)
{
    (void)args;
    app->modal = tui_modal_open(TUI_MODAL_CONFIRM_QUIT,
                                "Quit?", "Press y to quit, any other key to cancel.", NULL);
}
static void cmd_cancel_run(TuiApp *app, const char *args)
{
    (void)args;
    if (tui_worker_busy(app->worker)) tui_worker_cancel(app->worker);
}
static void cmd_submit_line(TuiApp *app, const char *args)
{
    (void)args;
    submit_line(app);
}
static void cmd_newline(TuiApp *app, const char *args)
{
    (void)args;
    tui_input_reset_history_walk(app->input);
    (void)tui_input_insert(app->input, "\n");
}
static void cmd_backspace(TuiApp *app, const char *args)
{
    (void)args;
    tui_input_reset_history_walk(app->input);
    (void)tui_input_backspace(app->input);
}
static void cmd_delete(TuiApp *app, const char *args)
{
    (void)args;
    tui_input_reset_history_walk(app->input);
    (void)tui_input_delete(app->input);
}
static void cmd_delete_word(TuiApp *app, const char *args)
{
    (void)args;
    tui_input_reset_history_walk(app->input);
    (void)tui_input_delete_word(app->input);
}
static void cmd_move_left(TuiApp *app, const char *args)
{
    (void)args;
    tui_input_move(app->input, -1);
}
static void cmd_move_right(TuiApp *app, const char *args)
{
    (void)args;
    tui_input_move(app->input, 1);
}
static void cmd_home(TuiApp *app, const char *args)
{
    (void)args;
    tui_input_home(app->input);
}
static void cmd_end(TuiApp *app, const char *args)
{
    (void)args;
    tui_input_end(app->input);
}
static void cmd_history_back(TuiApp *app, const char *args)
{
    (void)args;
    (void)tui_input_history_back(app->input);
}
static void cmd_history_forward(TuiApp *app, const char *args)
{
    (void)args;
    (void)tui_input_history_forward(app->input);
}

static void cmd_scroll_up(TuiApp *app, const char *args)
{
    (void)args;
    app->auto_scroll = 0;
    app->chat_top = app->chat_top > 5 ? app->chat_top - 5 : 0;
}
static void cmd_scroll_down(TuiApp *app, const char *args)
{
    (void)args;
    unsigned rows, cols;
    ncplane_dim_yx(app->chat_plane, &rows, &cols);
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
}
static void cmd_clear_chat(TuiApp *app, const char *args)
{
    (void)args;
    tui_chat_destroy(app->chat);
    app->chat = tui_chat_create();
    app->chat_top = 0;
}
static void cmd_session_list(TuiApp *app, const char *args)
{
    (void)args;
    slash_sessions(app, NULL);
}
static void cmd_session_new(TuiApp *app, const char *args)
{
    (void)args;
    (void)tui_worker_submit(app->worker, "new", NULL);
}
static void cmd_undo(TuiApp *app, const char *args)
{
    (void)args;
    int rc = ct_undo(app->ctx.ct);
    if (rc < 0) chat_notice(app, "Nothing to undo.");
    else chat_notice(app, "Undone.");
}
static void cmd_redo(TuiApp *app, const char *args)
{
    (void)args;
    int rc = ct_redo(app->ctx.ct);
    if (rc < 0) chat_notice(app, "Nothing to redo.");
    else chat_notice(app, "Redone.");
}
static void cmd_model_menu(TuiApp *app, const char *args)
{
    (void)args;
    if (app->pending_models_count > 0)
        open_model_picker(app);
    else
        open_provider_picker(app);
}
static void cmd_theme_menu(TuiApp *app, const char *args)
{
    (void)args;
    open_theme_picker(app);
}

/* ---- registry-driven dispatch ---- */

/* Dispatch a command by canonical name (keymap/palette path). */
void command_dispatch(TuiApp *app, const char *name, const char *args)
{
    const TuiCommand *c = tui_command_find(app->commands, name);
    if (c && c->fn)
    {
        c->fn(app, args);
        return;
    }
    log_error("tui: unregistered command '%s'", name);
}

/* Register every command. Handlers: existing slash_* take (TuiApp *,
 * const char *args) already; keymap-only actions use the cmd_* wrappers
 * above. The cast to TuiCommandFn is safe: the registry only ever
 * invokes handlers with a TuiApp * as ud. */
void commands_init(TuiApp *app)
{
    TuiCommandRegistry *r = tui_command_registry_create();
    if (!r)
    {
        log_error("tui: command registry unavailable", NULL);
        return;
    }
    app->commands = r;

#define REG(nm, tt, ds, cat, sl, al, sug, hid, hf) do {                       \
        TuiCommand c;                                                          \
        memset(&c, 0, sizeof(c));                                              \
        snprintf(c.name, sizeof(c.name), "%s", nm);                            \
        snprintf(c.title, sizeof(c.title), "%s", tt);                          \
        snprintf(c.desc, sizeof(c.desc), "%s", ds);                            \
        snprintf(c.category, sizeof(c.category), "%s", cat);                   \
        snprintf(c.slash, sizeof(c.slash), "%s", sl);                          \
        snprintf(c.aliases, sizeof(c.aliases), "%s", al);                      \
        c.suggested = (sug);                                                   \
        c.hidden = (hid);                                                      \
        c.fn = (TuiCommandFn)(hf);                                             \
        if (tui_command_register(r, &c) != 0)                                  \
            log_error("tui: command table full (%s)", nm);                     \
    } while (0)

    /* App / navigation */
    REG("app.exit", "Quit", "Exit the TUI", "App", "exit", "quit, q",
        0, 0, quit_immediate);
    REG("app.quit_prompt", "Quit", "Ask before quitting", "App", "", "",
        0, 1, cmd_quit_prompt);
    REG("command.palette.show", "Commands", "Browse and run every command", "App",
        "", "", 1, 0, open_command_palette);
    REG("app.menu", "Command menu", "Open the classic menu", "App", "menu", "",
        0, 0, cmd_open_menu);
    REG("app.help", "Help", "Show help", "App", "help", "", 1, 0, slash_help);
    REG("app.keybinds", "Keybindings", "Show the keymap", "App", "keybinds", "",
        0, 0, slash_keybinds);
    REG("app.status", "Status", "Show the status inventory", "App", "status", "",
        0, 0, slash_status);
    REG("app.debug", "Debug info", "Show environment details", "App", "debug", "",
        0, 0, slash_debug);
    REG("run.cancel", "Cancel run", "Cancel the running reply", "App", "", "",
        0, 1, cmd_cancel_run);

    /* Session */
    REG("session.new", "New session", "Reset the conversation", "Session", "new", "",
        1, 0, cmd_session_new);
    REG("session.clear", "Clear pane", "Clear the chat pane", "Chat", "clear", "",
        0, 0, cmd_clear_chat);
    REG("session.list", "Sessions", "List and search sessions", "Session",
        "sessions", "resume, continue", 1, 0, cmd_session_list);
    REG("session.undo", "Undo", "Undo the last file change", "Session", "undo", "",
        0, 0, cmd_undo);
    REG("session.redo", "Redo", "Redo the last undone change", "Session", "redo", "",
        0, 0, cmd_redo);
    REG("session.save", "Save session", "Save the session", "Session", "save", "",
        0, 0, slash_save);
    REG("session.load", "Load session", "Load a session", "Session", "load", "",
        0, 0, slash_load);
    REG("session.delete", "Delete session", "Delete a session", "Session",
        "delete", "", 0, 0, slash_delete);
    REG("session.rename", "Rename session", "Rename a session", "Session",
        "rename", "", 0, 0, slash_rename);
    REG("session.export", "Export session", "Export a session to markdown",
        "Session", "export", "", 0, 0, slash_export);
    REG("session.edit", "Edit message", "Edit a user message", "Session",
        "edit", "", 0, 0, slash_edit);
    REG("session.timeline", "Timeline", "Jump to a user message", "Session",
        "timeline", "", 0, 0, cmd_timeline);
    REG("session.fork", "Fork session", "Re-answer from a user message",
        "Session", "fork", "", 0, 0, cmd_fork);
    REG("session.regen", "Regenerate reply", "Regenerate an assistant reply",
        "Session", "regen", "", 0, 0, slash_regen);
    REG("session.branch", "Branches", "List or switch session branches",
        "Session", "branch", "", 0, 0, slash_branch);
    REG("session.compact", "Compact session", "Summarize the conversation",
        "Session", "compact", "summarize", 0, 0, cmd_compact);
    REG("session.copy", "Copy message", "Copy a message to the clipboard",
        "Chat", "copy", "", 0, 0, slash_copy);
    REG("session.stash", "Stash prompt", "Save the draft into the stash",
        "Chat", "stash", "", 0, 0, cmd_stash_push);
    REG("session.stash_pop", "Pop stash", "Restore the newest stashed draft",
        "Chat", "stash-pop", "stashp", 0, 0, cmd_stash_pop);
    REG("session.stash_list", "Stash list", "Browse stashed drafts",
        "Chat", "stash-list", "stashl", 0, 0, cmd_stash_list);
    REG("session.pin", "Pin session", "Pin or unpin the active session",
        "Session", "pin", "", 0, 0, cmd_session_pin);
    REG("session.sidebar.toggle", "Toggle sidebar", "Show or hide the sidebar",
        "Session", "", "", 0, 0, cmd_sidebar_toggle);
    REG("prompt.editor", "Edit in editor", "Open the draft in $EDITOR",
        "Prompt", "editor", "", 0, 0, cmd_editor);
    REG("prompt.complete", "Complete", "Complete a slash command",
        "Prompt", "", "", 0, 1, cmd_prompt_complete);
    REG("session.quick_switch.1", "Quick switch 1", "Load slot 1",
        "Session", "", "", 0, 1, cmd_quick_switch_1);
    REG("session.quick_switch.2", "Quick switch 2", "Load slot 2",
        "Session", "", "", 0, 1, cmd_quick_switch_2);
    REG("session.quick_switch.3", "Quick switch 3", "Load slot 3",
        "Session", "", "", 0, 1, cmd_quick_switch_3);
    REG("session.quick_switch.4", "Quick switch 4", "Load slot 4",
        "Session", "", "", 0, 1, cmd_quick_switch_4);
    REG("session.quick_switch.5", "Quick switch 5", "Load slot 5",
        "Session", "", "", 0, 1, cmd_quick_switch_5);
    REG("session.quick_switch.6", "Quick switch 6", "Load slot 6",
        "Session", "", "", 0, 1, cmd_quick_switch_6);
    REG("session.quick_switch.7", "Quick switch 7", "Load slot 7",
        "Session", "", "", 0, 1, cmd_quick_switch_7);
    REG("session.quick_switch.8", "Quick switch 8", "Load slot 8",
        "Session", "", "", 0, 1, cmd_quick_switch_8);
    REG("session.quick_switch.9", "Quick switch 9", "Load slot 9",
        "Session", "", "", 0, 1, cmd_quick_switch_9);

    /* Model / provider / theme */
    REG("model.list", "Model", "Pick provider or model", "Model", "", "",
        1, 0, cmd_model_menu);
    REG("model.set", "Switch model", "Switch the model", "Model", "model", "",
        0, 0, slash_model);
    REG("provider.set", "Switch provider", "Switch the provider", "Model",
        "provider", "", 0, 0, slash_provider);
    REG("model.effort", "Reasoning effort", "Set the reasoning effort", "Model",
        "effort", "", 0, 0, slash_effort);
    REG("model.cycle_recent", "Next recent model", "Cycle to the next recent model",
        "Model", "", "", 0, 1, cmd_model_cycle_recent);
    REG("model.cycle_recent_reverse", "Previous recent model",
        "Cycle to the previous recent model", "Model", "", "", 0, 1,
        cmd_model_cycle_recent_reverse);
    REG("theme.list", "Theme", "Pick a theme", "Theme", "", "", 0, 0, cmd_theme_menu);
    REG("theme.set", "Set theme", "Set the theme", "Theme", "theme", "",
        0, 0, slash_theme);

    /* Security / auth */
    REG("security.change_password", "Change password", "Change the database password",
        "Security", "change-password", "", 0, 0, slash_change_password);
    REG("security.lock", "Lock", "Lock the database", "Security", "lock", "",
        0, 0, slash_lock);
    REG("security.unlock", "Unlock", "Unlock the database", "Security", "unlock", "",
        0, 0, slash_unlock);
    REG("auth.openai_login", "OpenAI sign in", "Device-login to OpenAI",
        "Auth", "openai-login", "", 0, 0, slash_openai_login);
    REG("auth.openai_logout", "OpenAI sign out", "Sign out of OpenAI",
        "Auth", "openai-logout", "", 0, 0, slash_openai_logout);

    /* Input editing (keymap-only; hidden from the palette) */
    REG("input.submit", "Submit", "Submit the input", "Input", "", "", 0, 1,
        cmd_submit_line);
    REG("input.newline", "Newline", "Insert a newline", "Input", "", "", 0, 1,
        cmd_newline);
    REG("input.backspace", "Backspace", "Delete backward", "Input", "", "", 0, 1,
        cmd_backspace);
    REG("input.delete", "Delete", "Delete forward", "Input", "", "", 0, 1,
        cmd_delete);
    REG("input.delete_word", "Delete word", "Delete the word before the cursor",
        "Input", "", "", 0, 1, cmd_delete_word);
    REG("input.move.left", "Move left", "Move the cursor left", "Input", "", "",
        0, 1, cmd_move_left);
    REG("input.move.right", "Move right", "Move the cursor right", "Input", "", "",
        0, 1, cmd_move_right);
    REG("input.home", "Line home", "Jump to the line start", "Input", "", "",
        0, 1, cmd_home);
    REG("input.end", "Line end", "Jump to the line end", "Input", "", "", 0, 1,
        cmd_end);
    REG("input.history_back", "Previous prompt", "Walk prompt history back",
        "Input", "", "", 0, 1, cmd_history_back);
    REG("input.history_forward", "Next prompt", "Walk prompt history forward",
        "Input", "", "", 0, 1, cmd_history_forward);

    /* Chat scrolling (keymap-only) */
    REG("chat.scroll_up", "Scroll up", "Scroll toward older messages",
        "Chat", "", "", 0, 1, cmd_scroll_up);
    REG("chat.scroll_down", "Scroll down", "Scroll toward newer messages",
        "Chat", "", "", 0, 1, cmd_scroll_down);

#undef REG
}

/* Derive the prompt-store paths and seed the in-memory input history
 * from the persisted file, so history survives restarts. Best-effort:
 * failures just leave history empty (the live editor still works). */
void prompt_store_init(TuiApp *app)
{
    const char *home = getenv("HOME");
    if (!home) return;
    if (asprintf(&app->history_path,
                 "%s/.config/echo-ai/prompt-history.jsonl", home) < 0)
        return;
    if (asprintf(&app->stash_path,
                 "%s/.config/echo-ai/prompt-stash.jsonl", home) < 0)
    {
        free(app->history_path);
        app->history_path = NULL;
        return;
    }
    if (asprintf(&app->pins_path,
                 "%s/.config/echo-ai/sessions.json", home) < 0)
        app->pins_path = NULL; /* pins are optional; other stores survive */
    if (asprintf(&app->models_path,
                 "%s/.config/echo-ai/model-store.json", home) < 0)
        app->models_path = NULL;
    char **hist = NULL;
    int n = 0;
    if (tui_prompt_store_history_load(app->history_path, &hist, &n) == 0 &&
        n > 0)
    {
        tui_input_seed_history(app->input, (const char *const *)hist, n);
    }
    for (int i = 0; i < n; i++) free(hist[i]);
    free(hist);
}

void cmd_sidebar_toggle(TuiApp *app, const char *args)
{
    (void)args;
    app->sidebar_visible = !app->sidebar_visible;
    app->term_rows = 0; /* force the layout to rebuild for the new width */
}

/* Tab: complete a typed slash command. Only /-completion for now; @-file
 * completion is a documented follow-up (needs a file index + panel). */
void cmd_prompt_complete(TuiApp *app, const char *args)
{
    (void)args;
    const char *text = tui_input_text(app->input);
    if (!text || text[0] != '/') return;
    char out[128];
    if (tui_autocomplete_slash(app->commands, text, out, sizeof(out)))
    {
        tui_input_reset_history_walk(app->input);
        (void)tui_input_set_text(app->input, out);
    }
}

/* f2/shift+f2: cycle through the recently used models. */
static void cycle_recent_model(TuiApp *app, int forward)
{
    if (!app->models_path || !app->model)
    {
        chat_notice(app, "No models in the recent list yet.");
        return;
    }
    char **recent = NULL, **fav = NULL;
    int rn = 0, fn = 0;
    if (tui_model_store_load(app->models_path, &recent, &rn, &fav, &fn) != 0)
    {
        chat_notice(app, "Could not read the model store.");
        return;
    }
    int cur = -1;
    for (int i = 0; i < rn; i++)
        if (strcmp(recent[i], app->model) == 0) { cur = i; break; }
    int next = -1;
    if (rn > 0)
    {
        if (cur < 0)
            next = forward ? 0 : rn - 1;
        else
            next = (cur + (forward ? 1 : rn - 1)) % rn;
    }
    if (next >= 0 && next < rn)
        (void)tui_worker_submit(app->worker, "model", recent[next]);
    for (int i = 0; i < rn; i++) free(recent[i]);
    free(recent);
    for (int i = 0; i < fn; i++) free(fav[i]);
    free(fav);
}

void cmd_model_cycle_recent(TuiApp *app, const char *args)
{
    (void)args;
    cycle_recent_model(app, 1);
}

void cmd_model_cycle_recent_reverse(TuiApp *app, const char *args)
{
    (void)args;
    cycle_recent_model(app, 0);
}

/* ---- session pins + quick switch ---- */

/* /editor: open the current draft in $VISUAL (or $EDITOR). The TUI is
 * suspended (terminal restored, SIGINT defaulted) so the editor owns the
 * screen; the edited text replaces the input on return. Best-effort: any
 * failure leaves the draft untouched. */
static void cmd_editor(TuiApp *app, const char *args)
{
    (void)args;
    const char *ed = getenv("VISUAL");
    if (!ed || !ed[0]) ed = getenv("EDITOR");
    if (!ed || !ed[0])
    {
        chat_notice(app, "No $VISUAL or $EDITOR set.");
        return;
    }
    char tmpl[] = "/tmp/echo-ai-editor-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0)
    {
        chat_notice(app, "Could not create a temp file.");
        return;
    }
    const char *text = tui_input_text(app->input);
    size_t len = strlen(text);
    if (write(fd, text, len) != (ssize_t)len)
    {
        close(fd);
        unlink(tmpl);
        chat_notice(app, "Could not write the draft.");
        return;
    }
    close(fd);

    /* suspend the TUI so the editor can use the terminal directly */
    if (app->nc) (void)notcurses_stop(app->nc);
    app->nc = NULL;
    restore_signal_handlers(app->saved_sigs, 2);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "exec %s %s", ed, tmpl);
    pid_t pid = fork();
    int st = 0;
    if (pid == 0)
    {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) (void)waitpid(pid, &st, 0);

    install_signal_handlers(app->saved_sigs, 2);
    struct notcurses_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.flags = NCOPTION_SUPPRESS_BANNERS;
    app->nc = notcurses_init(&opts, NULL);
    app->status_plane = NULL; /* force layout_planes to rebuild all planes */
    app->term_rows = 0;

    if (app->nc && (st == 0 || WIFEXITED(st)))
    {
        char *buf = NULL;
        FILE *fp = fopen(tmpl, "r");
        if (fp)
        {
            long sz;
            if (fseek(fp, 0, SEEK_END) == 0 &&
                (sz = ftell(fp)) > 0 && (size_t)sz < (size_t)(1 << 20))
            {
                buf = malloc((size_t)sz + 1);
                if (buf)
                {
                    rewind(fp);
                    size_t got = fread(buf, 1, (size_t)sz, fp);
                    buf[got] = '\0';
                }
            }
            fclose(fp);
        }
        if (buf)
        {
            tui_input_reset_history_walk(app->input);
            (void)tui_input_set_text(app->input, buf);
            free(buf);
        }
    }
    unlink(tmpl);
    if (!app->nc)
        chat_notice(app, "Editor closed; terminal re-init failed.");
}

/* After an approval modal answers "Always", drop the tool from the
 * runtime approval list so future calls skip the prompt (until restart).
 * The dispatch already answered the worker with result 2 = allow; this
 * only persists the rule. */
void approval_commit(TuiApp *app)
{
    if (!app->modal || tui_modal_kind(app->modal) != TUI_MODAL_APPROVAL)
        return;
    if (app->modal->selection == 1 && app->ctx.safety)
    {
        const char *tool = tui_modal_title(app->modal);
        if (tool && tool[0])
        {
            (void)safety_allow_tool_always(app->ctx.safety, tool);
            set_status_msg(app, "Always allow set for this tool.");
        }
    }
}

void cmd_session_pin(TuiApp *app, const char *args)
{
    (void)args;
    if (!app->pins_path || !app->session_id)
    {
        chat_notice(app, "No active session to pin.");
        return;
    }
    int now = 0;
    if (tui_session_store_toggle_pin(app->pins_path, app->session_id, &now) == 0)
        chat_notice(app, now ? "Session pinned." : "Session unpinned.");
    else
        chat_notice(app, "Could not update pins.");
}

/* Load the session occupying a quick-switch slot: pinned sessions first
 * (pin order), then the remaining sessions by recency. */
static void quick_switch_slot(TuiApp *app, int slot)
{
    if (!app->ctx.sm)
    {
        chat_notice(app, "Session persistence disabled.");
        return;
    }
    char **pins = NULL;
    int pc = 0;
    if (tui_session_store_load_pins(app->pins_path, &pins, &pc) != 0)
    {
        chat_notice(app, "Could not read session pins.");
        return;
    }
    SessionList *list = session_manager_list_sessions(app->ctx.sm);
    if (!list)
    {
        for (int i = 0; i < pc; i++) free(pins[i]);
        free(pins);
        chat_notice(app, "No sessions found.");
        return;
    }
    char id[128];
    int ok = tui_session_store_resolve_slot(
        (const char *const *)pins, pc,
        (const char *const *)list->ids, list->count,
        slot, id, sizeof(id));
    for (int i = 0; i < pc; i++) free(pins[i]);
    free(pins);
    session_list_free(list);
    if (!ok)
    {
        chat_notice(app, "No session in that quick-switch slot.");
        return;
    }
    (void)tui_worker_submit(app->worker, "load", id);
}

#define DEFINE_QUICK_SWITCH(n) \
    void cmd_quick_switch_##n(TuiApp *app, const char *args) { (void)args; quick_switch_slot(app, n); }

DEFINE_QUICK_SWITCH(1)
DEFINE_QUICK_SWITCH(2)
DEFINE_QUICK_SWITCH(3)
DEFINE_QUICK_SWITCH(4)
DEFINE_QUICK_SWITCH(5)
DEFINE_QUICK_SWITCH(6)
DEFINE_QUICK_SWITCH(7)
DEFINE_QUICK_SWITCH(8)
DEFINE_QUICK_SWITCH(9)
#undef DEFINE_QUICK_SWITCH

/* ---- stash commands ---- */
