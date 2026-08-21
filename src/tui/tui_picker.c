/*
 * tui_picker.c - the TUI's picker dialogs: provider/model/menu/effort/
 * theme/session pickers plus their commit logic (picker_commit,
 * input_commit, confirm_commit). Split out of tui_cmd.c; these functions
 * drive the modal stack and the worker, never the agent directly.
 * Depends on: tui_internal.h, factory, session.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <ctype.h>

#include <notcurses/notcurses.h>

#include "tui_internal.h"
#include "tui_prompt_store.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"
#include "../tools/registry.h"
#include "../session/session.h"
#include "../llm/factory.h"
#include "../safety/safety.h"

#ifdef TUI_PICKER_TEST
/* Test-only mocks for the heavy externals (worker, chat, providers,
 * sessions, notcurses): the picker's commit/ownership logic is under
 * test, not those subsystems. Mirrors the REGISTRY_TEST precedent:
 * the define is only set by the test target, never in production. */
#define close_modal test_close_modal
#define set_status_msg test_set_status_msg
#define command_dispatch test_command_dispatch
#define slash_theme test_slash_theme
#define tui_worker_submit test_worker_submit
#define provider_names_available test_provider_names_available
#define provider_effort_options test_provider_effort_options
#define session_manager_list_sessions test_list_sessions
#define session_list_free test_session_list_free
#define tui_prompt_store_stash_list test_stash_list
#define tui_prompt_store_stash_pop test_stash_pop
#define tui_prompt_store_stash_push test_stash_push
#define tui_prompt_store_stash_remove test_stash_remove
#define tui_chat_append_error test_chat_append_error
#define tui_chat_block_count test_chat_block_count
#define tui_chat_block_kind test_chat_block_kind
#define tui_chat_block_text test_chat_block_text
#define tui_chat_block_render_lines test_chat_block_render_lines
#define ncplane_dim_yx test_ncplane_dim_yx
#define message_block_at test_message_block_at
#define registry_enabled_count test_registry_enabled_count

static char test_dispatched[256];
static char test_theme[64];
static char test_status[128];
static int test_worker_jobs[16];
static char test_worker_args[16][128];
static int test_worker_job_count;

static void test_close_modal(TuiApp *app, int answer_cancel)
{
    (void)answer_cancel;
    tui_modal_close(app->modal);
    app->modal = NULL;
}

static void test_set_status_msg(TuiApp *app, const char *msg)
{
    (void)app;
    if (msg) (void)snprintf(test_status, sizeof(test_status), "%s", msg);
}

static void test_command_dispatch(TuiApp *app, const char *name, const char *args)
{
    (void)app;
    (void)args;
    if (name) (void)snprintf(test_dispatched, sizeof(test_dispatched), "%s", name);
}

static void test_slash_theme(TuiApp *app, const char *style)
{
    (void)app;
    if (style) (void)snprintf(test_theme, sizeof(test_theme), "%s", style);
}

static int test_worker_submit(TuiWorker *worker, const char *job, const char *data)
{
    (void)worker;
    if (test_worker_job_count < 16 && job)
    {
        (void)snprintf(test_worker_args[test_worker_job_count],
                       sizeof(test_worker_args[0]), "%s", data ? data : "");
        test_worker_jobs[test_worker_job_count++] = (unsigned char)job[0];
    }
    return 0;
}

static const char *test_provider_names[4];
static const char *test_effort_opts[8];

static const char *const *test_provider_names_available(int *count)
{
    int n = 0;
    while (n < 3 && test_provider_names[n]) n++;
    if (count) *count = n;
    return test_provider_names;
}

static const char *const *test_provider_effort_options(const char *name)
{
    (void)name;
    return test_effort_opts[0] ? test_effort_opts : NULL;
}

static SessionList test_sessions;

static SessionList *test_list_sessions(SessionManager *sm)
{
    (void)sm;
    return &test_sessions;
}

static void test_session_list_free(SessionList *list)
{
    (void)list; /* the list itself is static test state */
}

static int test_stash_list(const char *path, char ***out, int *out_count)
{
    (void)path;
    (void)out;
    if (out_count) *out_count = 0;
    return 0;
}

static int test_stash_pop(const char *path, char **out)
{
    (void)path;
    if (out) *out = NULL;
    return 0;
}

static int test_stash_push(const char *path, const char *input, int max)
{
    (void)path;
    (void)input;
    (void)max;
    return 0;
}

static int test_stash_remove(const char *path, int index)
{
    (void)path;
    (void)index;
    return 0;
}

static int test_chat_append_error(TuiChat *chat, const char *text)
{
    (void)chat;
    (void)text;
    return 0;
}

static size_t test_chat_block_count(const TuiChat *chat)
{
    (void)chat;
    return 0;
}

static TuiBlockKind test_chat_block_kind(const TuiChat *chat, size_t idx)
{
    (void)chat;
    (void)idx;
    return TUI_BLOCK_USER;
}

static const char *test_chat_block_text(const TuiChat *chat, size_t idx)
{
    (void)chat;
    (void)idx;
    return "";
}

static size_t test_chat_block_render_lines(TuiChat *chat, size_t idx, size_t width)
{
    (void)chat;
    (void)idx;
    (void)width;
    return 0;
}

static void test_ncplane_dim_yx(const struct ncplane *n, unsigned *rows,
                                unsigned *cols)
{
    (void)n;
    if (rows) *rows = 0;
    if (cols) *cols = 0;
}

static long test_message_block_at(TuiApp *app, long n)
{
    (void)app;
    (void)n;
    return -1;
}

static int test_registry_enabled_count(void)
{
    return 0;
}

void tui_picker_test_reset(void)
{
    test_dispatched[0] = '\0';
    test_theme[0] = '\0';
    test_status[0] = '\0';
    test_worker_job_count = 0;
    test_provider_names[0] = NULL;
    test_provider_names[1] = NULL;
    test_provider_names[2] = NULL;
    test_provider_names[3] = NULL;
    test_effort_opts[0] = NULL;
    test_sessions.count = 0;
    test_sessions.ids = NULL;
    test_sessions.titles = NULL;
}

void tui_picker_test_set_sessions(char **ids, char **titles, int count)
{
    test_sessions.ids = ids;
    test_sessions.titles = titles;
    test_sessions.count = count;
}

const char *tui_picker_test_dispatched(void)
{
    return test_dispatched;
}

const char *tui_picker_test_theme(void)
{
    return test_theme;
}

int tui_picker_test_worker_jobs(void)
{
    return test_worker_job_count;
}

int tui_picker_test_worker_job(int index)
{
    return test_worker_jobs[index];
}

const char *tui_picker_test_worker_arg(int index)
{
    return test_worker_args[index];
}
#endif /* TUI_PICKER_TEST */

/* Free two caller-owned string arrays (model-store lists). */
static void free_str_lists(char **a, int an, char **b, int bn)
{
    for (int i = 0; i < an; i++) free(a[i]);
    free(a);
    for (int i = 0; i < bn; i++) free(b[i]);
    free(b);
}

void chat_notice(TuiApp *app, const char *text)
{
    (void)tui_chat_append_error(app->chat, text);
}

/* ---- Ctrl-P provider/model picker ---- */

void free_pending_models(TuiApp *app)
{
    for (int i = 0; i < app->pending_models_count; i++)
        free(app->pending_models[i]);
    free(app->pending_models);
    app->pending_models = NULL;
    app->pending_models_count = 0;
}

/* Join N strings with '\n' into a heap buffer ("" when count is 0). */
char *join_newline(const char *const *items, int count)
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
char *session_id_from_item(const char *item)
{
    if (!item) return NULL;
    const char *sp = strchr(item, ' ');
    return strndup(item, sp ? (size_t)(sp - item) : strlen(item));
}

void open_provider_picker(TuiApp *app)
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

void open_model_picker(TuiApp *app)
{
    if (app->pending_models_count == 0)
    {
        chat_notice(app, "No models available; use /model <name>.");
        app->picker_context = PICKER_NONE;
        return;
    }
    /* favorites and recent models lead the list (display markers only;
     * the picker's values carry the clean model ids) */
    char **fav = NULL, **recent = NULL;
    int fn = 0, rn = 0;
    if (app->models_path)
        (void)tui_model_store_load(app->models_path, &recent, &rn, &fav, &fn);

    size_t total = 1U;
    for (int i = 0; i < rn; i++) total += strlen(recent[i]) + 8;
    for (int i = 0; i < fn; i++) total += strlen(fav[i]) + 8;
    for (int i = 0; i < app->pending_models_count; i++)
        total += strlen(app->pending_models[i]) + 8;
    int items = rn + fn + app->pending_models_count;
    char *body = malloc(total);
    const char **values = calloc((size_t)(items > 0 ? items : 1), sizeof(char *));
    if (!body || !values)
    {
        free(body);
        free(values);
        free_str_lists(fav, fn, recent, rn);
        chat_notice(app, "Out of memory opening model picker.");
        app->picker_context = PICKER_NONE;
        return;
    }
    size_t pos = 0;
    int shown = 0;
    /* favorites first, then recent, then everything else */
    for (int i = 0; i < fn; i++)
    {
        pos += (size_t)snprintf(body + pos, pos < total ? total - pos : 0,
                                "%s\xE2\x98\x85 %s", pos > 0 ? "\n" : "", fav[i]);
        values[shown++] = fav[i]; // NOLINT(clang-analyzer-security.ArrayBound)
    }
    for (int i = 0; i < rn; i++)
    {
        int is_fav = 0;
        for (int j = 0; j < fn; j++)
            if (strcmp(fav[j], recent[i]) == 0) { is_fav = 1; break; }
        if (is_fav) continue; /* already listed */
        pos += (size_t)snprintf(body + pos, pos < total ? total - pos : 0,
                                "%s\xE2\x86\xBA %s", pos > 0 ? "\n" : "", recent[i]);
        values[shown++] = recent[i]; // NOLINT(clang-analyzer-security.ArrayBound)
    }
    for (int i = 0; i < app->pending_models_count; i++)
    {
        int dup = 0;
        for (int j = 0; j < shown; j++)
            if (strcmp(values[j], app->pending_models[i]) == 0) { dup = 1; break; }
        if (dup) continue;
        pos += (size_t)snprintf(body + pos, pos < total ? total - pos : 0,
                                "%s%s", pos > 0 ? "\n" : "",
                                app->pending_models[i]);
        values[shown++] = app->pending_models[i]; // NOLINT(clang-analyzer-security.ArrayBound)
    }
    free_pending_models(app);
    app->modal = tui_modal_open(TUI_MODAL_PICKER, "Select model", body, NULL);
    free(body);
    if (app->modal)
    {
        if (tui_modal_picker_set_values(app->modal, values, shown) != 0)
        {
            close_modal(app, 0);
            app->picker_context = PICKER_NONE;
            chat_notice(app, "Out of memory opening model picker.");
        }
    }
    else
    {
        app->picker_context = PICKER_NONE;
        chat_notice(app, "Out of memory opening model picker.");
    }
    free(values);
    free_str_lists(fav, fn, recent, rn);
}

/* ---- main menu (Ctrl-M) ---- */

void open_menu(TuiApp *app)
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

void open_effort_picker(TuiApp *app)
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

void open_theme_picker(TuiApp *app)
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

void open_session_picker(TuiApp *app, int context, const char *title)
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
void picker_menu_select(TuiApp *app, const char *sel)
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

void clear_menu_session(TuiApp *app)
{
    free(app->menu_session_id);
    app->menu_session_id = NULL;
}

/* Commit the picker selection. Returns 1 when a replacement modal was
 * opened (the caller must not close app->modal), 0 otherwise. */
int picker_commit(TuiApp *app)
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
    {
        const char *value = tui_modal_picker_selected_value(app->modal);
        if (value && value[0])
            (void)tui_worker_submit(app->worker, "model", value);
        else
            (void)tui_worker_submit(app->worker, "model", sel);
        app->picker_context = PICKER_NONE;
        return 0;
    }
    case PICKER_MENU:
        /* the selection lives in the modal's item array, which close_modal
         * frees, so copy it before closing the menu */
    {
        char *menu_sel = str_dup(sel);
        /* close the menu before opening the next dialog, so the menu
         * modal is not leaked and the ANSWERED close_modal skips cleanly
         * (return 1 below) */
        close_modal(app, 0);
        if (menu_sel)
            picker_menu_select(app, menu_sel);
        else
            app->picker_context = PICKER_NONE;
        free(menu_sel);
        return 1;
    }
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
        /* the session picker is replaced, not stacked: close it so the
         * confirm modal does not leak it */
        close_modal(app, 0);
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
        /* the session picker is replaced, not stacked: close it so the
         * input modal does not leak it */
        close_modal(app, 0);
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
        /* the session picker is replaced, not stacked: close it so the
         * input modal does not leak it */
        close_modal(app, 0);
        app->modal = tui_modal_open(TUI_MODAL_ASK_USER, "Rename session",
                                    "New name:", NULL);
        if (!app->modal)
        {
            app->input_action = INPUT_NONE;
            clear_menu_session(app);
        }
        return 1;
    case PICKER_COMMAND:
    {
        /* the value lives in the modal's values array, which close_modal
         * frees, so copy it before closing the palette */
        char *value = str_dup(tui_modal_picker_selected_value(app->modal));
        /* close the palette itself before running the command, so a
         * command that opens its own dialog (status/debug/themes/...) is
         * not instantly closed by the ANSWERED handler's close_modal and
         * the palette modal is not leaked */
        close_modal(app, 0);
        app->picker_context = PICKER_NONE;
        if (value && value[0])
            command_dispatch(app, value, NULL);
        free(value);
        return 1; /* the palette is already closed */
    }
    case PICKER_STASH:
    {
        const char *value = tui_modal_picker_selected_value(app->modal);
        int idx = tui_modal_picker_selected_index(app->modal);
        app->picker_context = PICKER_NONE;
        if (value && value[0] && app->stash_path && idx >= 0)
        {
            (void)tui_prompt_store_stash_remove(app->stash_path, idx);
            tui_input_reset_history_walk(app->input);
            (void)tui_input_set_text(app->input, value);
        }
        return 0;
    }
    case PICKER_TIMELINE:
    {
        const char *value = tui_modal_picker_selected_value(app->modal);
        app->picker_context = PICKER_NONE;
        if (value)
        {
            long n = strtol(value, NULL, 10);
            long blk = message_block_at(app, n);
            if (blk >= 0)
            {
                /* scroll so the selected message is at the top */
                unsigned rows, cols;
                ncplane_dim_yx(app->chat_plane, &rows, &cols);
                size_t cw = cols >= 6 ? (size_t)cols - 3 : (size_t)cols;
                size_t start = 0;
                for (long i = 0; i < blk; i++)
                    start += tui_chat_block_render_lines(app->chat, (size_t)i, cw) + 1;
                app->chat_top = start;
                app->auto_scroll = 0;
            }
        }
        return 0;
    }
    case PICKER_FORK:
    {
        const char *value = tui_modal_picker_selected_value(app->modal);
        app->picker_context = PICKER_NONE;
        if (value && value[0])
        {
            char *arg = NULL;
            if (asprintf(&arg, "%s", value) >= 0)
            {
                (void)tui_worker_submit(app->worker, "regen", arg);
                free(arg);
            }
        }
        return 0;
    }
    default:
        app->picker_context = PICKER_NONE;
        return 0;
    }
}

/* Commit the text collected by a menu-opened input modal. */
void input_commit(TuiApp *app)
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
void confirm_commit(TuiApp *app)
{
    int yes = app->modal->selection;
    int action = app->confirm_action;
    app->confirm_action = CONFIRM_NONE;
    if (action == CONFIRM_DELETE && yes && app->menu_session_id)
        (void)tui_worker_submit(app->worker, "delete", app->menu_session_id);
    clear_menu_session(app);
}
void open_command_palette(TuiApp *app, const char *args)
{
    (void)args;
    int n = tui_command_count(app->commands);
    if (n <= 0)
    {
        chat_notice(app, "No commands registered.");
        return;
    }
    /* Budget generously: each command row (title + keybind + padding),
     * a category header per category, plus the Suggested head. */
    size_t total = 1U;
    int max_rows = 0;
    int has_suggested = 0;
    for (int i = 0; i < n; i++)
    {
        const TuiCommand *c = tui_command_at(app->commands, i);
        if (c->hidden) continue;
        total += strlen(c->title) + strlen(c->category) + 80;
        max_rows++;
        if (c->suggested) has_suggested = 1;
    }
    total += (size_t)32 * 16; /* headers + slack */
    max_rows += 16;

    char *body = malloc(total);
    const char **values = calloc((size_t)max_rows, sizeof(char *));
    int *headers = calloc((size_t)max_rows, sizeof(int));
    if (!body || !values || !headers)
    {
        free(body);
        free(values);
        free(headers);
        chat_notice(app, "Out of memory opening the command palette.");
        return;
    }
    size_t pos = 0;
    int rows = 0;

    /* append helpers: a header row (value NULL) or a selectable command */
#define PAL_HEADER(txt) do {                                                  \
        pos += (size_t)snprintf(body + pos, total - pos, "%s%s",              \
                                pos > 0 ? "\n" : "", txt);                    \
        headers[rows] = 1;                                                    \
        values[rows] = NULL;                                                  \
        rows++;                                                               \
    } while (0)
#define PAL_CMD(c) do {                                                       \
        char line[200];                                                       \
        char keys[80] = "";                                                   \
        const TuiKeyBinding *kb = tui_keymap_binding(app->keymap, (c)->name); \
        if (kb && kb->enabled)                                                \
            snprintf(keys, sizeof(keys), "  [%s]", kb->keys);                 \
        snprintf(line, sizeof(line), "%s%s", (c)->title, keys);               \
        pos += (size_t)snprintf(body + pos, total - pos, "%s%s",              \
                                pos > 0 ? "\n" : "", line);                   \
        headers[rows] = 0;                                                    \
        values[rows] = (c)->name;                                             \
        rows++;                                                               \
    } while (0)

    /* Suggested head, then each category (in first-appearance order) */
    if (has_suggested)
    {
        PAL_HEADER("Suggested");
        for (int i = 0; i < n; i++)
        {
            const TuiCommand *c = tui_command_at(app->commands, i);
            if (c->hidden || !c->suggested) continue;
            PAL_CMD(c); // NOLINT(cert-err33-c)
        }
    }
    const char *cats[16];
    int ncat = 0;
    for (int i = 0; i < n; i++)
    {
        const TuiCommand *c = tui_command_at(app->commands, i);
        if (c->hidden || c->suggested) continue;
        int seen = 0;
        for (int j = 0; j < ncat; j++)
            if (strcmp(cats[j], c->category) == 0) { seen = 1; break; }
        if (!seen && ncat < 16)
            cats[ncat++] = c->category;
    }
    for (int ci = 0; ci < ncat; ci++)
    {
        PAL_HEADER(cats[ci]);
        for (int i = 0; i < n; i++)
        {
            const TuiCommand *c = tui_command_at(app->commands, i);
            if (c->hidden || c->suggested) continue;
            if (strcmp(c->category, cats[ci]) != 0) continue;
            PAL_CMD(c); // NOLINT(cert-err33-c)
        }
    }
#undef PAL_HEADER
#undef PAL_CMD

    if (rows == 0)
    {
        free(body);
        free(values);
        free(headers);
        chat_notice(app, "No commands available.");
        return;
    }
    app->picker_context = PICKER_COMMAND;
    app->modal = tui_modal_open(TUI_MODAL_PICKER, "Commands", body, NULL);
    free(body);
    if (app->modal)
    {
        int ok = tui_modal_picker_set_values(app->modal, values, rows) == 0 &&
                 tui_modal_picker_set_headers(app->modal, headers, rows) == 0;
        if (!ok)
        {
            close_modal(app, 0);
            app->picker_context = PICKER_NONE;
            chat_notice(app, "Out of memory opening the command palette.");
        }
    }
    else
    {
        app->picker_context = PICKER_NONE;
        chat_notice(app, "Out of memory opening the command palette.");
    }
    free(values);
    free(headers);
}

/* /status: a read-only inventory dialog. */
void slash_status(TuiApp *app, const char *args)
{
    (void)args;
    char lines[4][128];
    snprintf(lines[0], sizeof(lines[0]), "Provider: %s", // NOLINT(cert-err33-c)
             app->provider ? app->provider : "(none)");
    snprintf(lines[1], sizeof(lines[1]), "Model: %s", // NOLINT(cert-err33-c)
             app->model ? app->model : "(none)");
    snprintf(lines[2], sizeof(lines[2]), "Session: %s", // NOLINT(cert-err33-c)
             app->session_id ? app->session_id : "(none)");
    snprintf(lines[3], sizeof(lines[3]), "Enabled tools: %d", // NOLINT(cert-err33-c)
             registry_enabled_count());
    char *body = NULL;
    if (asprintf(&body, "%s\n%s\n%s\n%s",
                 lines[0], lines[1], lines[2], lines[3]) < 0)
    {
        chat_notice(app, "Out of memory opening status.");
        return;
    }
    app->picker_context = PICKER_STATUS;
    app->modal = tui_modal_open(TUI_MODAL_PICKER, "Status", body, NULL);
    free(body);
    if (!app->modal)
    {
        app->picker_context = PICKER_NONE;
        chat_notice(app, "Out of memory opening status.");
    }
}

/* Human-readable OS/terminal description for /debug (uname + env). */
static void describe_os(char *out, size_t cap)
{
    struct utsname u;
    if (cap < 1) return;
    if (uname(&u) != 0)
    {
        snprintf(out, cap, "unknown"); // NOLINT(cert-err33-c)
        return;
    }
    char buf[208];
    snprintf(buf, sizeof(buf), "%.*s %.*s (%.*s)", // NOLINT(cert-err33-c)
             (int)sizeof(u.sysname), u.sysname,
             (int)sizeof(u.release), u.release,
             (int)sizeof(u.machine), u.machine);
    strlcpy(out, buf, cap);
}

static void describe_terminal(char *out, size_t cap)
{
    if (cap < 1) return;
    const char *prog = getenv("TERM_PROGRAM");
    const char *term = getenv("TERM");
    if (prog && prog[0])
        snprintf(out, cap, "%s (%s)", prog, term ? term : "?"); // NOLINT(cert-err33-c)
    else
        snprintf(out, cap, "%s", term ? term : "unknown"); // NOLINT(cert-err33-c)
}

/* /debug: a copyable environment summary. */
void slash_debug(TuiApp *app, const char *args)
{
    (void)args;
    char os[160];
    describe_os(os, sizeof(os));
    char term[160];
    describe_terminal(term, sizeof(term));
    char *body = NULL;
    if (asprintf(&body,
                 "Version: " ECHO_AI_VERSION "\n"
                 "OS: %s\n"
                 "Terminal: %s\n"
                 "Session: %s\n"
                 "Model: %s\n"
                 "Provider: %s\n"
                 "Tool count: %d\n"
                 "Log: %s",
                 os, term,
                 app->session_id ? app->session_id : "n/a",
                 app->model ? app->model : "n/a",
                 app->provider ? app->provider : "n/a",
                 registry_enabled_count(),
                 app->log_path ? app->log_path : "n/a") < 0)
    {
        chat_notice(app, "Out of memory opening debug info.");
        return;
    }
    app->picker_context = PICKER_DEBUG;
    app->modal = tui_modal_open(TUI_MODAL_PICKER, "Debug", body, NULL);
    free(body);
    if (!app->modal)
    {
        app->picker_context = PICKER_NONE;
        chat_notice(app, "Out of memory opening debug info.");
    }
}

/* /compact: force a context-window summarization now. */
void cmd_compact(TuiApp *app, const char *args)
{
    (void)args;
    if (tui_worker_submit(app->worker, "compact", NULL) == -1)
        chat_notice(app, "Could not queue the compaction.");
}

void cmd_stash_push(TuiApp *app, const char *args)
{
    (void)args;
    if (!app->stash_path)
    {
        chat_notice(app, "Prompt stash unavailable (no state directory).");
        return;
    }
    const char *text = tui_input_text(app->input);
    if (!text || text[0] == '\0')
    {
        chat_notice(app, "Nothing to stash (input is empty).");
        return;
    }
    if (tui_prompt_store_stash_push(app->stash_path, text, 50) == 0)
    {
        tui_input_clear(app->input);
        chat_notice(app, "Prompt stashed.");
    }
    else
    {
        chat_notice(app, "Could not stash the prompt.");
    }
}

void cmd_stash_pop(TuiApp *app, const char *args)
{
    (void)args;
    if (!app->stash_path)
    {
        chat_notice(app, "Prompt stash unavailable (no state directory).");
        return;
    }
    char *popped = NULL;
    if (tui_prompt_store_stash_pop(app->stash_path, &popped) != 0)
    {
        chat_notice(app, "Could not pop the stash.");
        return;
    }
    if (!popped)
    {
        chat_notice(app, "Stash is empty.");
        return;
    }
    tui_input_reset_history_walk(app->input);
    (void)tui_input_set_text(app->input, popped);
    free(popped);
}

/* /stash list: a picker of stored drafts (newest first); selecting
 * restores that draft and removes it from the stash. */
void cmd_stash_list(TuiApp *app, const char *args)
{
    (void)args;
    if (!app->stash_path)
    {
        chat_notice(app, "Prompt stash unavailable (no state directory).");
        return;
    }
    char **list = NULL;
    int count = 0;
    if (tui_prompt_store_stash_list(app->stash_path, &list, &count) != 0)
    {
        chat_notice(app, "Could not read the stash.");
        return;
    }
    if (count <= 0)
    {
        free(list);
        chat_notice(app, "Stash is empty.");
        return;
    }
    size_t total = 1U;
    for (int i = 0; i < count; i++)
        total += strlen(list[i]) + 32;
    char *body = malloc(total);
    const char **values = calloc((size_t)count, sizeof(char *));
    if (!body || !values)
    {
        free(body);
        free(values);
        for (int i = 0; i < count; i++) free(list[i]);
        free(list);
        chat_notice(app, "Out of memory opening the stash.");
        return;
    }
    size_t pos = 0;
    for (int i = 0; i < count; i++)
    {
        /* show the first line, truncated; keep the full draft as value */
        char line[80];
        const char *nl = strchr(list[i], '\n');
        size_t len = nl ? (size_t)(nl - list[i]) : strlen(list[i]);
        if (len >= sizeof(line)) len = sizeof(line) - 2;
        memcpy(line, list[i], len);
        if (nl || list[i][len])
        {
            line[len] = '\xE2';
            line[len + 1] = '\x80';
            line[len + 2] = '\xA6'; // NOLINT(clang-analyzer-security.ArrayBound)
            line[len + 3] = '\0';
        }
        else
        {
            line[len] = '\0';
        }
        pos += (size_t)snprintf(body + pos, pos < total ? total - pos : 0,
                                "%s%s", pos > 0 ? "\n" : "", line);
        if (pos >= total) break; /* cannot happen: budgeted above */
        values[i] = list[i];
    }
    app->picker_context = PICKER_STASH;
    app->modal = tui_modal_open(TUI_MODAL_PICKER, "Stash", body, NULL);
    free(body);
    if (app->modal)
    {
        if (tui_modal_picker_set_values(app->modal, values, count) != 0)
        {
            close_modal(app, 0);
            app->picker_context = PICKER_NONE;
            chat_notice(app, "Out of memory opening the stash.");
        }
    }
    else
    {
        app->picker_context = PICKER_NONE;
        chat_notice(app, "Out of memory opening the stash.");
    }
    free(values);
    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
}

/* Build a picker of user messages for /timeline and /fork. Each row is
 * the first line of a user message; the commit value is its 1-based
 * position counting user+assistant messages (what regen/fork expect). */
static int open_message_picker(TuiApp *app, const char *title, int context)
{
    size_t count = tui_chat_block_count(app->chat);
    if (count == 0)
    {
        chat_notice(app, "No messages yet.");
        return 0;
    }
    /* first pass: count user messages and budget the body */
    int users = 0;
    long n = 0;
    size_t total = 1U;
    for (size_t i = 0; i < count; i++)
    {
        TuiBlockKind k = tui_chat_block_kind(app->chat, i);
        if (k == TUI_BLOCK_USER || k == TUI_BLOCK_ASSISTANT)
            n++;
        if (k == TUI_BLOCK_USER)
        {
            users++;
            const char *text = tui_chat_block_text(app->chat, i);
            total += strlen(text) + 32;
        }
    }
    if (users == 0)
    {
        chat_notice(app, "No user messages to jump to.");
        return 0;
    }
    char *body = malloc(total);
    const char **values = calloc((size_t)users, sizeof(char *));
    char **num_bufs = calloc((size_t)users, sizeof(char *));
    if (!body || !values || !num_bufs)
    {
        free(body); free(values); free(num_bufs);
        chat_notice(app, "Out of memory opening the timeline.");
        return 0;
    }
    size_t pos = 0;
    int shown = 0;
    n = 0;
    for (size_t i = 0; i < count && shown < users; i++)
    {
        TuiBlockKind k = tui_chat_block_kind(app->chat, i);
        if (k == TUI_BLOCK_USER || k == TUI_BLOCK_ASSISTANT)
            n++;
        if (k != TUI_BLOCK_USER)
            continue;
        const char *text = tui_chat_block_text(app->chat, i);
        char line[96];
        const char *nl = strchr(text, '\n');
        size_t len = nl ? (size_t)(nl - text) : strlen(text);
        if (len >= sizeof(line)) len = sizeof(line) - 2;
        memcpy(line, text, len);
        if (nl || text[len])
        {
            line[len] = '\xE2'; line[len + 1] = '\x80'; line[len + 2] = '\xA6'; // NOLINT(clang-analyzer-security.ArrayBound)
            line[len + 3] = '\0';
        }
        else
        {
            line[len] = '\0';
        }
        pos += (size_t)snprintf(body + pos, pos < total ? total - pos : 0,
                                "%s%ld. %s", pos > 0 ? "\n" : "", n, line);
        num_bufs[shown] = malloc(24);
        if (num_bufs[shown])
        {
            snprintf(num_bufs[shown], 24, "%ld", n); // NOLINT(cert-err33-c)
            values[shown] = num_bufs[shown];
        }
        shown++;
    }
    app->picker_context = context;
    app->modal = tui_modal_open(TUI_MODAL_PICKER, title, body, NULL);
    free(body);
    if (app->modal)
    {
        if (tui_modal_picker_set_values(app->modal, values, shown) != 0)
        {
            close_modal(app, 0);
            app->picker_context = PICKER_NONE;
            chat_notice(app, "Out of memory opening the timeline.");
        }
    }
    else
    {
        app->picker_context = PICKER_NONE;
        chat_notice(app, "Out of memory opening the timeline.");
    }
    free(values);
    for (int i = 0; i < users; i++) free(num_bufs[i]);
    free(num_bufs);
    return 1;
}

void cmd_timeline(TuiApp *app, const char *args)
{
    (void)args;
    (void)open_message_picker(app, "Timeline", PICKER_TIMELINE);
}

void cmd_fork(TuiApp *app, const char *args)
{
    (void)args;
    (void)open_message_picker(app, "Fork session", PICKER_FORK);
}
