/*
 * test_tui_dialogs.c - modal state machine: enter/esc semantics per kind,
 * approval cannot-dismiss invariant, password masking, and answer
 * round-trip wiring. Depends on: check, tui_events for round-trip events.
 */

#define _GNU_SOURCE
#include <check.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tui/tui_dialogs.h"
#include "tui/tui_events.h"
#include "tui/tui_input.h"

/* ---- password masking ---- */

START_TEST(test_mask_replaces_each_char)
{
    char out[64];
    tui_modal_mask("secret", out, sizeof(out));
    ck_assert_str_eq(out, "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");
}
END_TEST

START_TEST(test_mask_keeps_spaces)
{
    char out[64];
    tui_modal_mask("a b", out, sizeof(out));
    ck_assert_str_eq(out, "\xE2\x80\xA2 \xE2\x80\xA2");
}
END_TEST

START_TEST(test_mask_empty_and_null)
{
    char out[64];
    tui_modal_mask("", out, sizeof(out));
    ck_assert_str_eq(out, "");
    tui_modal_mask(NULL, out, sizeof(out));
    ck_assert_str_eq(out, "");
}
END_TEST

START_TEST(test_mask_truncates_to_capacity)
{
    char out[8];
    tui_modal_mask("abcdefghij", out, sizeof(out));
    ck_assert_int_lt(strlen(out), sizeof(out));
}
END_TEST

START_TEST(test_mask_multibyte_input)
{
    char out[64];
    tui_modal_mask("caf\xC3\xA9", out, sizeof(out)); /* café */
    /* one bullet per codepoint: 4 bullets */
    ck_assert_str_eq(out, "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");
}
END_TEST

/* ---- modal state machine ---- */

START_TEST(test_password_enter_submits_text)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_PASSWORD, "pw", "enter", NULL);
    ck_assert_ptr_nonnull(m);
    (void)tui_modal_dispatch_key(m, 'h');
    (void)tui_modal_dispatch_key(m, 'i');
    ck_assert_str_eq(tui_modal_text(m), "hi");
    ck_assert_int_eq(tui_modal_dispatch_key(m, 10), TUI_MODAL_ACTION_ANSWERED);
    tui_modal_close(m);
}
END_TEST

START_TEST(test_password_esc_cancels)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_PASSWORD, "pw", NULL, NULL);
    (void)tui_modal_dispatch_key(m, 'x');
    ck_assert_int_eq(tui_modal_dispatch_key(m, 27), TUI_MODAL_ACTION_ANSWERED);
    tui_modal_close(m);
}
END_TEST

START_TEST(test_password_backspace_edits)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_PASSWORD, "pw", NULL, NULL);
    (void)tui_modal_dispatch_key(m, 'a');
    (void)tui_modal_dispatch_key(m, 'b');
    (void)tui_modal_dispatch_key(m, 127);
    ck_assert_str_eq(tui_modal_text(m), "a");
    ck_assert_int_eq(tui_modal_dispatch_key(m, 10), TUI_MODAL_ACTION_ANSWERED);
    tui_modal_close(m);
}
END_TEST

START_TEST(test_confirm_quit_y_quits_other_keys_close)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_CONFIRM_QUIT, "q", NULL, NULL);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 'y'), TUI_MODAL_ACTION_QUIT);
    tui_modal_close(m);

    m = tui_modal_open(TUI_MODAL_CONFIRM_QUIT, "q", NULL, NULL);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 'n'), TUI_MODAL_ACTION_ANSWERED);
    tui_modal_close(m);

    m = tui_modal_open(TUI_MODAL_CONFIRM_QUIT, "q", NULL, NULL);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 27), TUI_MODAL_ACTION_ANSWERED);
    tui_modal_close(m);
}
END_TEST

START_TEST(test_approval_allow_deny)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_APPROVAL, "a", NULL, NULL);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 'y'), TUI_MODAL_ACTION_ANSWERED);
    tui_modal_close(m);

    m = tui_modal_open(TUI_MODAL_APPROVAL, "a", NULL, NULL);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 'n'), TUI_MODAL_ACTION_ANSWERED);
    tui_modal_close(m);
}
END_TEST

START_TEST(test_approval_rejects_random_keys)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_APPROVAL, "a", NULL, NULL);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 'x'), TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_dispatch_key(m, ' '), TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 12), TUI_MODAL_ACTION_NONE);
    tui_modal_close(m);
}
END_TEST

START_TEST(test_ask_user_submit_and_cancel)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_ASK_USER, "ask", "q?", NULL);
    (void)tui_modal_dispatch_key(m, 'o');
    (void)tui_modal_dispatch_key(m, 'k');
    ck_assert_str_eq(tui_modal_text(m), "ok");
    ck_assert_int_eq(tui_modal_dispatch_key(m, 10), TUI_MODAL_ACTION_ANSWERED);
    tui_modal_close(m);

    m = tui_modal_open(TUI_MODAL_ASK_USER, "ask", NULL, NULL);
    (void)tui_modal_dispatch_key(m, 'x');
    ck_assert_int_eq(tui_modal_dispatch_key(m, 27), TUI_MODAL_ACTION_ANSWERED);
    tui_modal_close(m);
}
END_TEST

START_TEST(test_notice_dismissed_by_any_key)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_NOTICE, "err", "wrong password", NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 'q'), TUI_MODAL_ACTION_ANSWERED);
    tui_modal_close(m);

    m = tui_modal_open(TUI_MODAL_NOTICE, "err", NULL, NULL);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 27), TUI_MODAL_ACTION_ANSWERED);
    tui_modal_close(m);
}
END_TEST

/* ---- round-trip answer wiring ---- */

typedef struct {
    TuiEvent *ev;
    int *result;
} WaitArgs;

static void *wait_thread(void *arg)
{
    WaitArgs *w = arg;
    (void)tui_event_wait_for_answer(w->ev, NULL, w->result);
    return NULL;
}

/* Run one approval modal against a real round-trip event and return the
 * worker-observed result. */
static int approval_result_with(const char *keys, size_t nkeys)
{
    TuiEvents *evs = tui_events_init(4);
    ck_assert_ptr_nonnull(evs);
    TuiEvent *ev = tui_events_push(evs, TUI_EV_APPROVAL, "bash", "{}");
    ck_assert_ptr_nonnull(ev);
    ck_assert_ptr_eq(tui_events_pop(evs), ev);

    int result = -1;
    WaitArgs args = {.ev = ev, .result = &result};
    pthread_t t;
    ck_assert_int_eq(pthread_create(&t, NULL, wait_thread, &args), 0);
    usleep(50000);
    TuiModal *m = tui_modal_open(TUI_MODAL_APPROVAL, "a", NULL, ev);
    for (size_t i = 0; i < nkeys; i++)
        (void)tui_modal_dispatch_key(m, keys[i]);
    tui_modal_close(m);
    pthread_join(t, NULL);

    tui_event_free(ev);
    tui_events_destroy(evs);
    return result;
}

START_TEST(test_approval_allow_once_via_enter)
{
    /* default selection is "Allow once": plain Enter -> allow (1) */
    ck_assert_int_eq(approval_result_with((const char[]){10}, 1), 1);
}
END_TEST

START_TEST(test_approval_always_via_navigation)
{
    /* navigate right once to "Always", Enter -> allow with result 2 */
    ck_assert_int_eq(approval_result_with((const char[]){'l', 10}, 2), 2);
}
END_TEST

START_TEST(test_approval_reject_via_navigation)
{
    /* navigate to "Reject" (right twice), Enter -> deny (0) */
    ck_assert_int_eq(approval_result_with((const char[]){'l', 'l', 10}, 3), 0);
}
END_TEST

START_TEST(test_approval_reject_via_esc_never_allows)
{
    ck_assert_int_eq(approval_result_with((const char[]){27}, 1), 0);
}
END_TEST

START_TEST(test_approval_quick_allow_and_reject_keys)
{
    ck_assert_int_eq(approval_result_with((const char[]){'y'}, 1), 1);
    ck_assert_int_eq(approval_result_with((const char[]){'n'}, 1), 0);
}
END_TEST

START_TEST(test_approval_selection_moves)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_APPROVAL, "a", NULL, NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(m->selection, 0); /* defaults to Allow once */
    (void)tui_modal_dispatch_key(m, 'l');
    ck_assert_int_eq(m->selection, 1); /* Always */
    (void)tui_modal_dispatch_key(m, 'l');
    ck_assert_int_eq(m->selection, 2); /* Reject */
    (void)tui_modal_dispatch_key(m, 'l');
    ck_assert_int_eq(m->selection, 0); /* wraps */
    (void)tui_modal_dispatch_key(m, 'h');
    ck_assert_int_eq(m->selection, 2); /* backwards */
    (void)tui_modal_dispatch_key(m, '2');
    ck_assert_int_eq(m->selection, 1); /* direct select */
    tui_modal_close(m);
}
END_TEST

START_TEST(test_approval_event_gets_decision)
{
    TuiEvents *evs = tui_events_init(4);
    ck_assert_ptr_nonnull(evs);
    TuiEvent *ev = tui_events_push(evs, TUI_EV_APPROVAL, "bash", "{}");
    ck_assert_ptr_nonnull(ev);
    ck_assert_ptr_eq(tui_events_pop(evs), ev);

    /* worker side waits, UI side dispatches the modal */
    int result = -1;
    WaitArgs args = {.ev = ev, .result = &result};
    pthread_t t;
    ck_assert_int_eq(pthread_create(&t, NULL, wait_thread, &args), 0);
    usleep(50000);
    TuiModal *m = tui_modal_open(TUI_MODAL_APPROVAL, "a", NULL, ev);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 'n'), TUI_MODAL_ACTION_ANSWERED);
    tui_modal_close(m);
    pthread_join(t, NULL);
    ck_assert_int_eq(result, 0); /* denied */

    tui_event_free(ev);
    tui_events_destroy(evs);
}
END_TEST

/* ---- picker list selection ---- */

START_TEST(test_picker_opens_with_items)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_PICKER, "Pick", "ollama\nopenai\nzen",
                                 NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(tui_modal_picker_item_count(m), 3);
    ck_assert_int_eq(tui_modal_picker_visible_count(m), 3);
    ck_assert_str_eq(tui_modal_picker_visible_at(m, 0), "ollama");
    ck_assert_str_eq(tui_modal_picker_visible_at(m, 2), "zen");
    ck_assert_int_eq(tui_modal_picker_cursor(m), 0);
    tui_modal_close(m);
}

END_TEST

START_TEST(test_picker_empty_body_opens_empty)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_PICKER, "Pick", "", NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(tui_modal_picker_item_count(m), 0);
    ck_assert_int_eq(tui_modal_picker_visible_count(m), 0);
    tui_modal_close(m);
}

END_TEST

START_TEST(test_picker_enter_selects_cursor_item)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_PICKER, "Pick", "ollama\nopenai\nzen",
                                 NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(tui_modal_dispatch_key(m, TUI_PICKER_KEY_DOWN),
                     TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_dispatch_key(m, TUI_PICKER_KEY_DOWN),
                     TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_picker_cursor(m), 2);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 10), TUI_MODAL_ACTION_ANSWERED);
    ck_assert_str_eq(tui_modal_picker_selected(m), "zen");
    tui_modal_close(m);
}

END_TEST

START_TEST(test_picker_up_clamps_at_top)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_PICKER, "Pick", "a\nb", NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(tui_modal_dispatch_key(m, TUI_PICKER_KEY_UP),
                     TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_picker_cursor(m), 0);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 10), TUI_MODAL_ACTION_ANSWERED);
    ck_assert_str_eq(tui_modal_picker_selected(m), "a");
    tui_modal_close(m);
}

END_TEST

START_TEST(test_picker_esc_cancels)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_PICKER, "Pick", "a\nb", NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 27), TUI_MODAL_ACTION_ANSWERED);
    ck_assert_ptr_null(tui_modal_picker_selected(m));
    tui_modal_close(m);
}

END_TEST

START_TEST(test_picker_filter_restricts_and_selects)
{
    /* Type "op": only the two "open*" entries remain and cursor clamps. */
    TuiModal *m = tui_modal_open(TUI_MODAL_PICKER, "Pick", "ollama\nopenai\nopencode_zen",
                                 NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 'o'), TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 'p'), TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_picker_visible_count(m), 2);
    ck_assert_str_eq(tui_modal_picker_visible_at(m, 0), "openai");
    ck_assert_str_eq(tui_modal_picker_visible_at(m, 1), "opencode_zen");
    ck_assert_int_eq(tui_modal_dispatch_key(m, 10), TUI_MODAL_ACTION_ANSWERED);
    ck_assert_str_eq(tui_modal_picker_selected(m), "openai");
    tui_modal_close(m);
}

END_TEST

START_TEST(test_picker_filter_empty_selects_first)
{
    /* A filter with no matches must not select out of bounds: Enter on an
     * empty visible list cancels (selection stays NULL). */
    TuiModal *m = tui_modal_open(TUI_MODAL_PICKER, "Pick", "ollama\nopenai", NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 'z'), TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_picker_visible_count(m), 0);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 10), TUI_MODAL_ACTION_ANSWERED);
    ck_assert_ptr_null(tui_modal_picker_selected(m));
    tui_modal_close(m);
}

END_TEST

START_TEST(test_picker_backspace_restores_filter)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_PICKER, "Pick", "ollama\nopenai", NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 'o'), TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 127), TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_picker_visible_count(m), 2);
    tui_modal_close(m);
}

END_TEST

START_TEST(test_picker_ctrl_p_n_navigate)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_PICKER, "Pick", "a\nb\nc", NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 16), TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_picker_cursor(m), 0); /* up at top clamps */
    ck_assert_int_eq(tui_modal_dispatch_key(m, 14), TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_picker_cursor(m), 1); /* ctrl-n moves down */
    tui_modal_close(m);
}

END_TEST

START_TEST(test_picker_window_scrolls_top)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_PICKER, "Pick",
                                 "m1\nm2\nm3\nm4\nm5\nm6\nm7\nm8", NULL);
    ck_assert_ptr_nonnull(m);
    tui_modal_picker_set_window(m, 3);
    ck_assert_int_eq(tui_modal_dispatch_key(m, TUI_PICKER_KEY_DOWN),
                     TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_dispatch_key(m, TUI_PICKER_KEY_DOWN),
                     TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_dispatch_key(m, TUI_PICKER_KEY_DOWN),
                     TUI_MODAL_ACTION_NONE);
    /* cursor 3 with window 3: top must scroll to 1 to keep it visible */
    ck_assert_int_eq(tui_modal_picker_cursor(m), 3);
    ck_assert_int_eq(tui_modal_picker_top(m), 1);
    tui_modal_close(m);
}

END_TEST

/* ---- generic confirm modal ---- */

START_TEST(test_confirm_y_records_yes)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_CONFIRM, "Delete", "Sure?", NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 'y'), TUI_MODAL_ACTION_ANSWERED);
    ck_assert_int_eq(m->selection, 1);
    tui_modal_close(m);
}

END_TEST

START_TEST(test_confirm_n_and_esc_record_no)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_CONFIRM, "Delete", "Sure?", NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 'n'), TUI_MODAL_ACTION_ANSWERED);
    ck_assert_int_eq(m->selection, 0);
    tui_modal_close(m);

    m = tui_modal_open(TUI_MODAL_CONFIRM, "Delete", "Sure?", NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 27), TUI_MODAL_ACTION_ANSWERED);
    ck_assert_int_eq(m->selection, 0);
    tui_modal_close(m);
}

END_TEST

START_TEST(test_confirm_other_keys_ignored)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_CONFIRM, "Delete", "Sure?", NULL);
    ck_assert_ptr_nonnull(m);
    ck_assert_int_eq(tui_modal_dispatch_key(m, 'x'), TUI_MODAL_ACTION_NONE);
    ck_assert_int_eq(tui_modal_dispatch_key(m, ' '), TUI_MODAL_ACTION_NONE);
    tui_modal_close(m);
}

END_TEST

START_TEST(test_picker_headers_skip_and_never_commit)
{
    /* rows: header, item, header, item — headers are skipped by the
     * cursor and never committed */
    TuiModal *m = tui_modal_open(TUI_MODAL_PICKER, "Pick",
                                 "Session\nNew\nModel\nList", NULL);
    ck_assert_ptr_nonnull(m);
    int flags[] = { 1, 0, 1, 0 };
    ck_assert_int_eq(tui_modal_picker_set_headers(m, flags, 4), 0);
    ck_assert_int_eq(tui_modal_picker_item_count(m), 4);

    /* the cursor lands on the first selectable row (index 1), not the
     * leading header */
    ck_assert_int_eq(tui_modal_picker_visible_is_header(m, 0), 1);
    ck_assert_int_eq(tui_modal_picker_visible_is_header(m, 1), 0);
    ck_assert_int_eq(tui_modal_picker_cursor(m), 1);

    /* navigation skips headers: down from item 1 goes to item 3 */
    (void)tui_modal_dispatch_key(m, TUI_PICKER_KEY_DOWN);
    ck_assert_int_eq(tui_modal_picker_cursor(m), 3);
    (void)tui_modal_dispatch_key(m, TUI_PICKER_KEY_UP);
    ck_assert_int_eq(tui_modal_picker_cursor(m), 1);

    /* committing selects a real item, never a header */
    ck_assert_int_eq(tui_modal_dispatch_key(m, 10), TUI_MODAL_ACTION_ANSWERED);
    ck_assert_str_eq(tui_modal_picker_selected(m), "New");
    tui_modal_close(m);
}
END_TEST

START_TEST(test_picker_header_stays_when_child_matches)
{
    /* filtering keeps a header only while one of its children matches */
    TuiModal *m = tui_modal_open(TUI_MODAL_PICKER, "Pick",
                                 "App\nQuit\nSession\nSave", NULL);
    ck_assert_ptr_nonnull(m);
    int flags[] = { 1, 0, 1, 0 };
    ck_assert_int_eq(tui_modal_picker_set_headers(m, flags, 4), 0);

    (void)tui_modal_dispatch_key(m, 's'); /* matches "Save" under Session */
    ck_assert_int_eq(tui_modal_picker_visible_count(m), 2); /* header + Save */
    ck_assert_int_eq(tui_modal_picker_visible_is_header(m, 0), 1);
    ck_assert_str_eq(tui_modal_picker_visible_at(m, 1), "Save");
    tui_modal_close(m);
}
END_TEST

START_TEST(test_picker_set_headers_bad_count_rejected)
{
    TuiModal *m = tui_modal_open(TUI_MODAL_PICKER, "Pick", "a\nb", NULL);
    ck_assert_ptr_nonnull(m);
    int flags[] = { 1, 0, 0 };
    ck_assert_int_eq(tui_modal_picker_set_headers(m, flags, 3), -1); /* != 2 */
    ck_assert_int_eq(tui_modal_picker_set_headers(m, NULL, 0), -1);
    tui_modal_close(m);
}
END_TEST

static Suite *suite(void)
{
    Suite *s = suite_create("tui_dialogs");
    TCase *tc_mask = tcase_create("mask");
    tcase_add_test(tc_mask, test_mask_replaces_each_char);
    tcase_add_test(tc_mask, test_mask_keeps_spaces);
    tcase_add_test(tc_mask, test_mask_empty_and_null);
    tcase_add_test(tc_mask, test_mask_truncates_to_capacity);
    tcase_add_test(tc_mask, test_mask_multibyte_input);
    suite_add_tcase(s, tc_mask);

    TCase *tc_state = tcase_create("state_machine");
    tcase_add_test(tc_state, test_password_enter_submits_text);
    tcase_add_test(tc_state, test_password_esc_cancels);
    tcase_add_test(tc_state, test_password_backspace_edits);
    tcase_add_test(tc_state, test_confirm_quit_y_quits_other_keys_close);
    tcase_add_test(tc_state, test_approval_allow_deny);
    tcase_add_test(tc_state, test_approval_rejects_random_keys);
    tcase_add_test(tc_state, test_ask_user_submit_and_cancel);
    tcase_add_test(tc_state, test_notice_dismissed_by_any_key);
    suite_add_tcase(s, tc_state);

    TCase *tc_rt = tcase_create("roundtrip");
    tcase_add_test(tc_rt, test_approval_event_gets_decision);
    tcase_add_test(tc_rt, test_approval_allow_once_via_enter);
    tcase_add_test(tc_rt, test_approval_always_via_navigation);
    tcase_add_test(tc_rt, test_approval_reject_via_navigation);
    tcase_add_test(tc_rt, test_approval_reject_via_esc_never_allows);
    tcase_add_test(tc_rt, test_approval_quick_allow_and_reject_keys);
    tcase_add_test(tc_rt, test_approval_selection_moves);
    tcase_set_timeout(tc_rt, 10);
    suite_add_tcase(s, tc_rt);

    TCase *tc_picker = tcase_create("picker");
    tcase_add_test(tc_picker, test_picker_opens_with_items);
    tcase_add_test(tc_picker, test_picker_empty_body_opens_empty);
    tcase_add_test(tc_picker, test_picker_enter_selects_cursor_item);
    tcase_add_test(tc_picker, test_picker_up_clamps_at_top);
    tcase_add_test(tc_picker, test_picker_esc_cancels);
    tcase_add_test(tc_picker, test_picker_filter_restricts_and_selects);
    tcase_add_test(tc_picker, test_picker_filter_empty_selects_first);
    tcase_add_test(tc_picker, test_picker_backspace_restores_filter);
    tcase_add_test(tc_picker, test_picker_ctrl_p_n_navigate);
    tcase_add_test(tc_picker, test_picker_window_scrolls_top);
    tcase_add_test(tc_picker, test_picker_headers_skip_and_never_commit);
    tcase_add_test(tc_picker, test_picker_header_stays_when_child_matches);
    tcase_add_test(tc_picker, test_picker_set_headers_bad_count_rejected);
    suite_add_tcase(s, tc_picker);

    TCase *tc_confirm = tcase_create("confirm");
    tcase_add_test(tc_confirm, test_confirm_y_records_yes);
    tcase_add_test(tc_confirm, test_confirm_n_and_esc_record_no);
    tcase_add_test(tc_confirm, test_confirm_other_keys_ignored);
    suite_add_tcase(s, tc_confirm);
    return s;
}

int main(void)
{
    Suite *s = suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? 0 : 1;
}
