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
    tcase_set_timeout(tc_rt, 10);
    suite_add_tcase(s, tc_rt);
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
