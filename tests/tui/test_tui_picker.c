/*
 * test_tui_picker.c - picker commit ownership: the selected item and
 * value live in the modal's arrays, which close_modal frees. Commits
 * that read them after closing are use-after-free (the ASan heap-UAF in
 * picker_commit, .config/echo-ai/tui.log); commits that replace the
 * modal without closing it leak the old one (9 leaked modals in the
 * same log). Depends on: check, tui_dialogs, the TUI_PICKER_TEST mocks.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>

#include "tui/tui_internal.h"
#include "tui/tui_dialogs.h"

/* test hooks exported by tui_picker.c under TUI_PICKER_TEST */
void tui_picker_test_reset(void);
void tui_picker_test_set_sessions(char **ids, char **titles, int count);
const char *tui_picker_test_dispatched(void);
const char *tui_picker_test_theme(void);
int tui_picker_test_worker_jobs(void);
int tui_picker_test_worker_job(int index);
const char *tui_picker_test_worker_arg(int index);

static TuiApp test_app;
static SessionManager dummy_sm; /* never dereferenced: list_sessions is stubbed */

/* A zeroed TuiApp is enough: picker_commit only touches the modal,
 * picker context, and the fields the mocks record. open_session_picker
 * still guards on ctx.sm, so point it at a dummy that the stub ignores. */
static void reset_app(void)
{
    memset(&test_app, 0, sizeof(test_app));
    test_app.ctx.sm = &dummy_sm;
    tui_picker_test_reset();
}

/* Drive a picker to the row at index i and press Enter. */
static void pick_row(int index)
{
    for (int k = 0; k < index; k++)
        (void)tui_modal_dispatch_key(test_app.modal, TUI_PICKER_KEY_DOWN);
    (void)tui_modal_dispatch_key(test_app.modal, 10);
}

START_TEST(test_command_palette_commit_uses_value_after_modal_close)
{
    reset_app();
    test_app.picker_context = PICKER_COMMAND;
    test_app.modal = tui_modal_open(TUI_MODAL_PICKER, "Commands",
                                    "one\ntwo", NULL);
    ck_assert_ptr_nonnull(test_app.modal);
    /* command values live in the modal's values array: commit must copy
     * them before close_modal frees the array (was a heap-use-after-free
     * at picker_commit, read of the strdup'd 13-byte value) */
    const char *values[2] = {"one", "two"};
    ck_assert_int_eq(tui_modal_picker_set_values(test_app.modal, values, 2), 0);
    pick_row(1);

    ck_assert_int_eq(picker_commit(&test_app), 1);
    ck_assert_ptr_null(test_app.modal);
    ck_assert_str_eq(tui_picker_test_dispatched(), "two");
}
END_TEST

START_TEST(test_menu_commit_uses_selection_after_modal_close)
{
    reset_app();
    open_menu(&test_app);
    ck_assert_ptr_nonnull(test_app.modal);
    /* "Change password" (row 2): picker_menu_select strcmp's the item,
     * which the menu close frees — must be copied first */
    pick_row(2);

    ck_assert_int_eq(picker_commit(&test_app), 1);
    ck_assert_ptr_null(test_app.modal);
    ck_assert_int_eq(test_app.menu_action, MENU_ACTION_CHANGE_PASSWORD);
}
END_TEST

START_TEST(test_menu_theme_commit_opens_replacement_picker)
{
    reset_app();
    open_menu(&test_app);
    pick_row(5); /* "Theme" */

    ck_assert_int_eq(picker_commit(&test_app), 1);
    /* the menu closed and the theme picker replaced it in app->modal */
    ck_assert_ptr_nonnull(test_app.modal);
    ck_assert_int_eq(tui_modal_kind(test_app.modal), TUI_MODAL_PICKER);
    ck_assert_int_eq(test_app.picker_context, PICKER_THEME);
    ck_assert_int_eq(test_app.modal->item_count, 4);
}
END_TEST

START_TEST(test_menu_delete_session_opens_session_picker)
{
    reset_app();
    char *ids[1] = {(char *)"s1"};
    char *titles[1] = {(char *)"first"};
    tui_picker_test_set_sessions(ids, titles, 1);
    open_menu(&test_app);
    pick_row(3); /* "Delete session" */

    ck_assert_int_eq(picker_commit(&test_app), 1);
    ck_assert_ptr_nonnull(test_app.modal);
    ck_assert_int_eq(tui_modal_kind(test_app.modal), TUI_MODAL_PICKER);
    ck_assert_int_eq(test_app.picker_context, PICKER_SESSION_DELETE);
    /* rows are rendered "id  title" from the stub session list */
    ck_assert_int_eq(test_app.modal->item_count, 1);
    ck_assert_str_eq(test_app.modal->items[0], "s1  first");
}
END_TEST

START_TEST(test_session_delete_commit_replaces_modal)
{
    reset_app();
    /* a session picker with one "id  title" row, selection committed */
    test_app.picker_context = PICKER_SESSION_DELETE;
    test_app.modal = tui_modal_open(TUI_MODAL_PICKER, "Delete session",
                                    "abc123  My session", NULL);
    ck_assert_ptr_nonnull(test_app.modal);
    pick_row(0);

    ck_assert_int_eq(picker_commit(&test_app), 1);
    /* the confirm modal replaced the picker; the picker must have been
     * closed, not overwritten (that leaked 9 modals in the tui.log run) */
    ck_assert_ptr_nonnull(test_app.modal);
    ck_assert_int_eq(tui_modal_kind(test_app.modal), TUI_MODAL_CONFIRM);
    ck_assert_int_eq(test_app.confirm_action, CONFIRM_DELETE);
    ck_assert_str_eq(test_app.menu_session_id, "abc123");
}
END_TEST

START_TEST(test_session_export_commit_replaces_modal)
{
    reset_app();
    test_app.picker_context = PICKER_SESSION_EXPORT;
    test_app.modal = tui_modal_open(TUI_MODAL_PICKER, "Export session",
                                    "abc123  My session", NULL);
    ck_assert_ptr_nonnull(test_app.modal);
    pick_row(0);

    ck_assert_int_eq(picker_commit(&test_app), 1);
    ck_assert_ptr_nonnull(test_app.modal);
    ck_assert_int_eq(tui_modal_kind(test_app.modal), TUI_MODAL_ASK_USER);
    ck_assert_int_eq(test_app.input_action, INPUT_EXPORT_PATH);
    ck_assert_str_eq(test_app.menu_session_id, "abc123");
}
END_TEST

START_TEST(test_session_rename_commit_replaces_modal)
{
    reset_app();
    test_app.picker_context = PICKER_SESSION_RENAME;
    test_app.modal = tui_modal_open(TUI_MODAL_PICKER, "Rename session",
                                    "abc123  My session", NULL);
    ck_assert_ptr_nonnull(test_app.modal);
    pick_row(0);

    ck_assert_int_eq(picker_commit(&test_app), 1);
    ck_assert_ptr_nonnull(test_app.modal);
    ck_assert_int_eq(tui_modal_kind(test_app.modal), TUI_MODAL_ASK_USER);
    ck_assert_int_eq(test_app.input_action, INPUT_RENAME_NAME);
    ck_assert_str_eq(test_app.menu_session_id, "abc123");
}
END_TEST

START_TEST(test_session_load_commit_submits_worker_job)
{
    reset_app();
    /* the /load no-args flow opens this picker (slash_load → session
     * picker): committing must submit the picked id to the worker */
    test_app.picker_context = PICKER_SESSION_LOAD;
    test_app.modal = tui_modal_open(TUI_MODAL_PICKER, "Load session",
                                    "abc123  My session", NULL);
    ck_assert_ptr_nonnull(test_app.modal);
    pick_row(0);

    ck_assert_int_eq(picker_commit(&test_app), 0);
    ck_assert_int_eq(test_app.picker_context, PICKER_NONE);
    ck_assert_int_eq(tui_picker_test_worker_jobs(), 1);
    ck_assert_int_eq(tui_picker_test_worker_job(0), 'l');
    ck_assert_str_eq(tui_picker_test_worker_arg(0), "abc123");
    tui_modal_close(test_app.modal);
}
END_TEST

START_TEST(test_empty_selection_cancels_everything)
{
    reset_app();
    test_app.picker_context = PICKER_MENU;
    test_app.modal = tui_modal_open(TUI_MODAL_PICKER, "Menu", "one\n", NULL);
    ck_assert_ptr_nonnull(test_app.modal);
    (void)tui_modal_dispatch_key(test_app.modal, 27); /* Esc: selection -1 */

    ck_assert_int_eq(picker_commit(&test_app), 0);
    ck_assert_int_eq(test_app.picker_context, PICKER_NONE);
    tui_modal_close(test_app.modal);
}
END_TEST

static Suite *suite(void)
{
    Suite *s = suite_create("tui_picker");
    TCase *tc_commit = tcase_create("commit_ownership");
    tcase_add_test(tc_commit, test_command_palette_commit_uses_value_after_modal_close);
    tcase_add_test(tc_commit, test_menu_commit_uses_selection_after_modal_close);
    tcase_add_test(tc_commit, test_menu_theme_commit_opens_replacement_picker);
    tcase_add_test(tc_commit, test_menu_delete_session_opens_session_picker);
    tcase_add_test(tc_commit, test_session_delete_commit_replaces_modal);
    tcase_add_test(tc_commit, test_session_export_commit_replaces_modal);
    tcase_add_test(tc_commit, test_session_rename_commit_replaces_modal);
    tcase_add_test(tc_commit, test_session_load_commit_submits_worker_job);
    tcase_add_test(tc_commit, test_empty_selection_cancels_everything);
    suite_add_tcase(s, tc_commit);
    return s;
}

int main(void)
{
    int failed = 0;
    SRunner *sr = srunner_create(suite());
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? 0 : 1;
}
