/*
 * tui_internal.h - shared private surface of the TUI app: the TuiApp
 * struct and the helper functions shared between tui.c (app shell),
 * tui_render.c (rendering) and tui_cmd.c (commands/pickers). NOT a
 * public contract: nothing outside src/tui/ may include this header.
 * Depends on: every module that TuiApp embeds or touches.
 */

#ifndef ECHO_TUI_INTERNAL_H
#define ECHO_TUI_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <signal.h>

#include <notcurses/notcurses.h>

#include "tui.h"
#include "tui_theme.h"
#include "tui_chat.h"
#include "tui_input.h"
#include "tui_dialogs.h"
#include "tui_keys.h"
#include "tui_command.h"
#include "tui_worker.h"
#include "tui_prompt_store.h"
#include "tui_session_store.h"
#include "tui_model_store.h"
#include "tui_autocomplete.h"
#include "../utils/string_utils.h"
#include "../utils/logging.h"
#include "../tools/registry.h"
#include "../session/session.h"
#include "../llm/factory.h"

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
    PICKER_SESSION_RENAME, /* session list -> rename (name input) */
    PICKER_KEYBINDS,   /* read-only: keymap listing; commit just closes */
    PICKER_COMMAND,    /* command palette: commit dispatches the command */
    PICKER_STATUS,     /* read-only: status inventory */
    PICKER_DEBUG,      /* read-only: debug info */
    PICKER_STASH,      /* stash list: commit restores + removes the draft */
    PICKER_TIMELINE,   /* user-message list: commit scrolls to the block */
    PICKER_FORK        /* user-message list: commit forks/regens at it */
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

struct TuiApp {
    TuiAppCtx ctx;
    struct notcurses *nc;
    struct ncplane *status_plane;
    struct ncplane *chat_plane;
    struct ncplane *sidebar_plane;
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
    TuiKeymap *keymap;
    TuiCommandRegistry *commands;
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
    char *history_path; /* prompt-history.jsonl (owned); NULL if unavailable */
    char *stash_path;   /* prompt-stash.jsonl (owned); NULL if unavailable */
    char *pins_path;    /* sessions.json pins (owned); NULL if unavailable */
    char *models_path;  /* model store json (owned); NULL if unavailable */
    size_t chat_top;  /* scroll offset in lines */
    int auto_scroll;
    int sidebar_visible;  /* 1: the session sidebar is shown */
    unsigned input_rows;  /* input plane height (multiline growth) */
    unsigned input_rows_laid; /* input height the current planes were built for */
    size_t frame;     /* spinner frame counter */
    int quit;
    unsigned term_rows, term_cols;
    struct sigaction saved_sigs[2]; /* SIGINT/SIGTERM, restored on destroy */
};

/* Shared helpers (implemented in tui.c unless noted). */
void chat_notice(TuiApp *app, const char *text);
void set_status_msg(TuiApp *app, const char *msg);
void close_modal(TuiApp *app, int answer_cancel);
void drain_events(TuiApp *app);
void free_pending_models(TuiApp *app);
void clear_menu_session(TuiApp *app);
void handle_command(TuiApp *app, const char *cmd);
void command_dispatch(TuiApp *app, const char *name, const char *args);
void submit_line(TuiApp *app);
void handle_click(TuiApp *app, const ncinput *ni);
void render_all(TuiApp *app);
char *join_newline(const char *const *items, int count);
char *session_id_from_item(const char *item);
int picker_commit(TuiApp *app);
void input_commit(TuiApp *app);
void confirm_commit(TuiApp *app);
uint64_t mono_now_ms(void);

/* Implemented in tui_picker.c (pick + dialog pickers). */
void open_provider_picker(TuiApp *app);
void open_model_picker(TuiApp *app);
void open_menu(TuiApp *app);
void open_effort_picker(TuiApp *app);
void open_theme_picker(TuiApp *app);
void open_session_picker(TuiApp *app, int context, const char *title);
void picker_menu_select(TuiApp *app, const char *sel);
void open_command_palette(TuiApp *app, const char *args);
void slash_status(TuiApp *app, const char *args);
void slash_debug(TuiApp *app, const char *args);
void cmd_compact(TuiApp *app, const char *args);
void cmd_stash_push(TuiApp *app, const char *args);
void cmd_stash_pop(TuiApp *app, const char *args);
void cmd_stash_list(TuiApp *app, const char *args);

/* Implemented in tui_cmd.c. */
void commands_init(TuiApp *app);
void prompt_store_init(TuiApp *app);
void rebuild_chat(TuiApp *app, const char *json);
void slash_change_password(TuiApp *app, const char *args);
void slash_theme(TuiApp *app, const char *style);
void cmd_session_pin(TuiApp *app, const char *args);
void cmd_quick_switch_1(TuiApp *app, const char *args);
void cmd_quick_switch_2(TuiApp *app, const char *args);
void cmd_quick_switch_3(TuiApp *app, const char *args);
void cmd_quick_switch_4(TuiApp *app, const char *args);
void cmd_quick_switch_5(TuiApp *app, const char *args);
void cmd_quick_switch_6(TuiApp *app, const char *args);
void cmd_quick_switch_7(TuiApp *app, const char *args);
void cmd_quick_switch_8(TuiApp *app, const char *args);
void cmd_quick_switch_9(TuiApp *app, const char *args);
void cmd_sidebar_toggle(TuiApp *app, const char *args);
void approval_commit(TuiApp *app);
void cmd_timeline(TuiApp *app, const char *args);
void cmd_fork(TuiApp *app, const char *args);
void cmd_prompt_complete(TuiApp *app, const char *args);
long message_block_at(TuiApp *app, long n);

/* Implemented in tui.c (app shell). */
struct ncplane *make_plane(TuiApp *app, unsigned rows, unsigned cols,
                           int y, int x);
int install_signal_handlers(struct sigaction *saved, size_t n_saved);
void restore_signal_handlers(const struct sigaction *saved, size_t n_saved);

/* Implemented in tui_render.c. */
int approval_bar_lines(const TuiApp *app, unsigned cols);

#endif /* ECHO_TUI_INTERNAL_H */
