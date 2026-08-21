/*
 * test_tui_keys.c - keymap model: key-string parsing, stroke equality,
 * binding registration/override/disable, and the leader-chord engine
 * (arming, chord resolution, timeout, cancel). Depends on: check.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tui/tui_keys.h"

/* TuiKeyBinding fields are fixed arrays; fill them safely. */
#define BIND_FIELD(field, val) do { snprintf((field), sizeof(field), "%s", (val)); } while (0)

/* ---- key parsing ---- */

START_TEST(test_parse_single_char)
{
    int alts;
    int lens[TUI_KEYSEQ_MAX_ALT];
    TuiKeyStroke s[TUI_KEYSEQ_MAX_ALT][TUI_KEYSEQ_MAX_KEYS];
    ck_assert_int_eq(tui_key_parse("x", &alts, lens, s), 0);
    ck_assert_int_eq(alts, 1);
    ck_assert_int_eq(lens[0], 1);
    ck_assert_int_eq(s[0][0].id, (uint32_t)'x');
    ck_assert_int_eq(s[0][0].ctrl, 0);
}
END_TEST

START_TEST(test_parse_modifiers)
{
    int alts;
    int lens[TUI_KEYSEQ_MAX_ALT];
    TuiKeyStroke s[TUI_KEYSEQ_MAX_ALT][TUI_KEYSEQ_MAX_KEYS];
    ck_assert_int_eq(tui_key_parse("ctrl+x", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, (uint32_t)'x');
    ck_assert_int_eq(s[0][0].ctrl, 1);
    ck_assert_int_eq(s[0][0].shift, 0);

    ck_assert_int_eq(tui_key_parse("shift+tab", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_TAB);
    ck_assert_int_eq(s[0][0].shift, 1);

    ck_assert_int_eq(tui_key_parse("ctrl+shift+x", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, (uint32_t)'x');
    ck_assert_int_eq(s[0][0].ctrl, 1);
    ck_assert_int_eq(s[0][0].shift, 1);
}
END_TEST

START_TEST(test_parse_key_names)
{
    int alts;
    int lens[TUI_KEYSEQ_MAX_ALT];
    TuiKeyStroke s[TUI_KEYSEQ_MAX_ALT][TUI_KEYSEQ_MAX_KEYS];
    ck_assert_int_eq(tui_key_parse("enter", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_ENTER);
    ck_assert_int_eq(tui_key_parse("return", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_ENTER);
    ck_assert_int_eq(tui_key_parse("esc", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_ESC);
    ck_assert_int_eq(tui_key_parse("escape", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_ESC);
    ck_assert_int_eq(tui_key_parse("backspace", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_BACKSPACE);
    ck_assert_int_eq(tui_key_parse("del", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_DEL);
    ck_assert_int_eq(tui_key_parse("delete", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_DEL);
    ck_assert_int_eq(tui_key_parse("pgup", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_PGUP);
    ck_assert_int_eq(tui_key_parse("pageup", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_PGUP);
    ck_assert_int_eq(tui_key_parse("pgdown", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_PGDOWN);
    ck_assert_int_eq(tui_key_parse("space", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_SPACE);
    ck_assert_int_eq(tui_key_parse("f2", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_F2);
    ck_assert_int_eq(tui_key_parse("f12", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_F12);
    ck_assert_int_eq(tui_key_parse("f13", &alts, lens, s), -1); /* out of range */
}
END_TEST

START_TEST(test_parse_alternatives_and_chord)
{
    int alts;
    int lens[TUI_KEYSEQ_MAX_ALT];
    TuiKeyStroke s[TUI_KEYSEQ_MAX_ALT][TUI_KEYSEQ_MAX_KEYS];
    ck_assert_int_eq(tui_key_parse("ctrl+c,ctrl+d,leader+q", &alts, lens, s), 0);
    ck_assert_int_eq(alts, 3);
    ck_assert_int_eq(lens[0], 1);
    ck_assert_int_eq(s[0][0].ctrl, 1);
    ck_assert_int_eq(s[0][0].id, (uint32_t)'c');
    ck_assert_int_eq(s[1][0].ctrl, 1);
    ck_assert_int_eq(s[1][0].id, (uint32_t)'d');
    ck_assert_int_eq(s[2][0].id, TUI_KEYID_LEADER);
    ck_assert_int_eq(s[2][1].id, (uint32_t)'q');
    ck_assert_int_eq(lens[2], 2);

    /* "<leader>m" spelling is equivalent */
    ck_assert_int_eq(tui_key_parse("<leader>m", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, TUI_KEYID_LEADER);
    ck_assert_int_eq(s[0][1].id, (uint32_t)'m');
    ck_assert_int_eq(lens[0], 2);
}
END_TEST

START_TEST(test_parse_errors)
{
    int alts;
    int lens[TUI_KEYSEQ_MAX_ALT];
    TuiKeyStroke s[TUI_KEYSEQ_MAX_ALT][TUI_KEYSEQ_MAX_KEYS];
    ck_assert_int_eq(tui_key_parse("boguskey", &alts, lens, s), -1);
    ck_assert_int_eq(tui_key_parse("ctrll+x", &alts, lens, s), -1);
    ck_assert_int_eq(tui_key_parse("", &alts, lens, s), -1);
    ck_assert_int_eq(tui_key_parse("ctrl+", &alts, lens, s), -1);
}
END_TEST

START_TEST(test_stroke_equality)
{
    TuiKeyStroke a = { (uint32_t)'x', 1, 0, 0, 0, 0 };
    TuiKeyStroke b = { (uint32_t)'x', 1, 0, 0, 0, 0 };
    TuiKeyStroke c = { (uint32_t)'x', 0, 0, 0, 0, 0 };
    TuiKeyStroke d = { (uint32_t)'y', 1, 0, 0, 0, 0 };
    ck_assert_int_eq(tui_key_stroke_equal(&a, &b), 1);
    ck_assert_int_eq(tui_key_stroke_equal(&a, &c), 0);
    ck_assert_int_eq(tui_key_stroke_equal(&a, &d), 0);
}
END_TEST

START_TEST(test_stroke_to_string)
{
    char buf[64];
    TuiKeyStroke a = { (uint32_t)'x', 1, 0, 0, 0, 0 };
    tui_key_stroke_to_string(&a, buf, sizeof(buf));
    ck_assert_str_eq(buf, "ctrl+x");
    TuiKeyStroke b = { TUI_KEYID_TAB, 0, 1, 0, 0, 0 };
    tui_key_stroke_to_string(&b, buf, sizeof(buf));
    ck_assert_str_eq(buf, "shift+tab");
    TuiKeyStroke c = { TUI_KEYID_ENTER, 0, 0, 0, 0, 0 };
    tui_key_stroke_to_string(&c, buf, sizeof(buf));
    ck_assert_str_eq(buf, "enter");
    TuiKeyStroke d = { TUI_KEYID_F2, 0, 0, 0, 0, 0 };
    tui_key_stroke_to_string(&d, buf, sizeof(buf));
    ck_assert_str_eq(buf, "f2");
}
END_TEST

/* ---- keymap dispatch ---- */

#define MK(id, c, sh) ((TuiKeyStroke){ (uint32_t)(id), (c), (sh), 0, 0, 0 })

static void register_defaults(TuiKeymap *km)
{
    TuiKeyBinding b;
    memset(&b, 0, sizeof(b));
    BIND_FIELD(b.name, "app.exit"); BIND_FIELD(b.category, "App");
    BIND_FIELD(b.desc, "Quit the app"); BIND_FIELD(b.keys, "ctrl+c,leader+q");
    ck_assert_int_eq(tui_keymap_register(km, &b), 0);
    BIND_FIELD(b.name, "menu.open"); BIND_FIELD(b.category, "App");
    BIND_FIELD(b.desc, "Open the menu"); BIND_FIELD(b.keys, "ctrl+m,ctrl+p");
    ck_assert_int_eq(tui_keymap_register(km, &b), 0);
    BIND_FIELD(b.name, "session.list"); BIND_FIELD(b.category, "Session");
    BIND_FIELD(b.desc, "List sessions"); BIND_FIELD(b.keys, "leader+l");
    ck_assert_int_eq(tui_keymap_register(km, &b), 0);
    BIND_FIELD(b.name, "chat.clear"); BIND_FIELD(b.category, "Chat");
    BIND_FIELD(b.desc, "Clear the pane"); BIND_FIELD(b.keys, "ctrl+l");
    ck_assert_int_eq(tui_keymap_register(km, &b), 0);
}

START_TEST(test_dispatch_single_keys)
{
    TuiKeymap *km = tui_keymap_create();
    ck_assert_ptr_nonnull(km);
    register_defaults(km);

    const char *name = NULL;
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('c', 1, 0), 0, &name), TUI_KEYMAP_CMD);
    ck_assert_str_eq(name, "app.exit");
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('m', 1, 0), 0, &name), TUI_KEYMAP_CMD);
    ck_assert_str_eq(name, "menu.open");
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('p', 1, 0), 0, &name), TUI_KEYMAP_CMD);
    ck_assert_str_eq(name, "menu.open");
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('l', 1, 0), 0, &name), TUI_KEYMAP_CMD);
    ck_assert_str_eq(name, "chat.clear");

    /* unmatched key is NOMATCH (caller treats as free input) */
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('a', 0, 0), 0, &name), TUI_KEYMAP_NOMATCH);

    /* modifiers distinguish: plain 'c' is not ctrl+c */
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('c', 0, 0), 0, &name), TUI_KEYMAP_NOMATCH);

    tui_keymap_destroy(km);
}
END_TEST

START_TEST(test_leader_arms_and_resolves)
{
    TuiKeymap *km = tui_keymap_create();
    ck_assert_ptr_nonnull(km);
    register_defaults(km);

    const char *name = NULL;
    /* leader press arms the chord */
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('x', 1, 0), 1000, &name), TUI_KEYMAP_LEADER);
    ck_assert_int_eq(tui_keymap_leader_active(km), 1);

    /* completing the chord dispatches the command */
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('l', 0, 0), 1100, &name), TUI_KEYMAP_CMD);
    ck_assert_str_eq(name, "session.list");
    ck_assert_int_eq(tui_keymap_leader_active(km), 0);
    tui_keymap_destroy(km);
}
END_TEST

START_TEST(test_leader_timeout)
{
    TuiKeymap *km = tui_keymap_create();
    ck_assert_ptr_nonnull(km);
    tui_keymap_set_leader_timeout(km, 2000);
    register_defaults(km);

    const char *name = NULL;
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('x', 1, 0), 1000, &name), TUI_KEYMAP_LEADER);

    /* before the deadline the chord is still armed */
    ck_assert_int_eq(tui_keymap_leader_expired(km, 2500), 0);
    ck_assert_int_eq(tui_keymap_leader_active(km), 1);

    /* after the deadline it expires and disarms */
    ck_assert_int_eq(tui_keymap_leader_expired(km, 3001), 1);
    ck_assert_int_eq(tui_keymap_leader_active(km), 0);

    /* a fresh dispatch after expiry treats the next key normally */
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('x', 1, 0), 4000, &name), TUI_KEYMAP_LEADER);
    tui_keymap_leader_clear(km);
    ck_assert_int_eq(tui_keymap_leader_active(km), 0);
    tui_keymap_destroy(km);
}
END_TEST

START_TEST(test_leader_miss_falls_through)
{
    TuiKeymap *km = tui_keymap_create();
    ck_assert_ptr_nonnull(km);
    register_defaults(km);

    const char *name = NULL;
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('x', 1, 0), 1000, &name), TUI_KEYMAP_LEADER);

    /* 'q' is a leader chord for app.exit (leader+q); typing plain 'z'
     * disarms the leader and is retried as a fresh single key (NOMATCH) */
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('z', 0, 0), 1100, &name), TUI_KEYMAP_NOMATCH);
    ck_assert_int_eq(tui_keymap_leader_active(km), 0);

    /* the retried key could still hit a single-key binding */
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('x', 1, 0), 1200, &name), TUI_KEYMAP_LEADER);
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('q', 0, 0), 1300, &name), TUI_KEYMAP_CMD);
    ck_assert_str_eq(name, "app.exit");
    tui_keymap_destroy(km);
}
END_TEST

START_TEST(test_bind_override_and_disable)
{
    TuiKeymap *km = tui_keymap_create();
    ck_assert_ptr_nonnull(km);
    register_defaults(km);

    const char *name = NULL;
    ck_assert_int_eq(tui_keymap_bind(km, "chat.clear", "ctrl+k"), 0);
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('l', 1, 0), 0, &name), TUI_KEYMAP_NOMATCH);
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('k', 1, 0), 0, &name), TUI_KEYMAP_CMD);
    ck_assert_str_eq(name, "chat.clear");

    ck_assert_int_eq(tui_keymap_bind(km, "chat.clear", "none"), 0);
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('k', 1, 0), 0, &name), TUI_KEYMAP_NOMATCH);

    ck_assert_int_eq(tui_keymap_disable(km, "menu.open"), 0);
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('m', 1, 0), 0, &name), TUI_KEYMAP_NOMATCH);

    /* unknown names fail */
    ck_assert_int_eq(tui_keymap_bind(km, "no.such.command", "ctrl+k"), -1);
    ck_assert_int_eq(tui_keymap_disable(km, "no.such.command"), -1);
    tui_keymap_destroy(km);
}
END_TEST

START_TEST(test_bad_registration_spec_fails)
{
    TuiKeymap *km = tui_keymap_create();
    ck_assert_ptr_nonnull(km);
    TuiKeyBinding b;
    memset(&b, 0, sizeof(b));
    BIND_FIELD(b.name, "bad"); BIND_FIELD(b.category, "App");
    BIND_FIELD(b.desc, "bad"); BIND_FIELD(b.keys, "boguskey");
    ck_assert_int_eq(tui_keymap_register(km, &b), -1);
    ck_assert_ptr_null(tui_keymap_binding(km, "bad"));
    tui_keymap_destroy(km);
}
END_TEST

START_TEST(test_register_keeps_override_state)
{
    TuiKeymap *km = tui_keymap_create();
    ck_assert_ptr_nonnull(km);
    register_defaults(km);
    ck_assert_int_eq(tui_keymap_disable(km, "chat.clear"), 0);

    /* re-registering the same name keeps the disabled state */
    TuiKeyBinding b;
    memset(&b, 0, sizeof(b));
    BIND_FIELD(b.name, "chat.clear"); BIND_FIELD(b.category, "Chat");
    BIND_FIELD(b.desc, "Clear"); BIND_FIELD(b.keys, "ctrl+l");
    ck_assert_int_eq(tui_keymap_register(km, &b), 0);
    const char *name = NULL;
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('l', 1, 0), 0, &name), TUI_KEYMAP_NOMATCH);
    tui_keymap_destroy(km);
}
END_TEST

struct walk_ud { int count; int saw_disabled; };

static void walk_cb(void *ud, const TuiKeyBinding *b)
{
    struct walk_ud *w = ud;
    w->count++;
    if (!b->enabled) w->saw_disabled = 1;
}

START_TEST(test_walk_and_lookup)
{
    TuiKeymap *km = tui_keymap_create();
    ck_assert_ptr_nonnull(km);
    register_defaults(km);
    ck_assert_int_eq(tui_keymap_disable(km, "menu.open"), 0);

    struct walk_ud w = { 0, 0 };
    tui_keymap_walk(km, walk_cb, &w);
    ck_assert_int_eq(w.count, 4);
    ck_assert_int_eq(w.saw_disabled, 1);

    const TuiKeyBinding *b = tui_keymap_binding(km, "session.list");
    ck_assert_ptr_nonnull(b);
    ck_assert_str_eq(b->name, "session.list");
    ck_assert_str_eq(b->keys, "leader+l");
    ck_assert_ptr_null(tui_keymap_binding(km, "nope"));
    tui_keymap_destroy(km);
}
END_TEST

START_TEST(test_custom_leader)
{
    TuiKeymap *km = tui_keymap_create();
    ck_assert_ptr_nonnull(km);
    ck_assert_int_eq(tui_keymap_set_leader(km, "ctrl+b"), 0);

    TuiKeyBinding b;
    memset(&b, 0, sizeof(b));
    BIND_FIELD(b.name, "session.new"); BIND_FIELD(b.category, "Session");
    BIND_FIELD(b.desc, "New"); BIND_FIELD(b.keys, "leader+n");
    ck_assert_int_eq(tui_keymap_register(km, &b), 0);

    const char *name = NULL;
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('b', 1, 0), 0, &name), TUI_KEYMAP_LEADER);
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('n', 0, 0), 100, &name), TUI_KEYMAP_CMD);
    ck_assert_str_eq(name, "session.new");

    /* old ctrl+x leader no longer arms */
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('x', 1, 0), 200, &name), TUI_KEYMAP_NOMATCH);
    tui_keymap_destroy(km);
}
END_TEST

START_TEST(test_count_and_at)
{
    TuiKeymap *km = tui_keymap_create();
    ck_assert_ptr_nonnull(km);
    ck_assert_int_eq(tui_keymap_count(km), 0);
    register_defaults(km);
    ck_assert_int_eq(tui_keymap_count(km), 4);

    const TuiKeyBinding *b0 = tui_keymap_at(km, 0);
    ck_assert_ptr_nonnull(b0);
    ck_assert_str_eq(b0->name, "app.exit");
    ck_assert_ptr_null(tui_keymap_at(km, 4));   /* one past the end */
    ck_assert_ptr_null(tui_keymap_at(km, -1));
    tui_keymap_destroy(km);
}
END_TEST

START_TEST(test_normalize_case)
{
    /* terminal layers report the shifted glyph: Ctrl+P arrives as 'P' with
     * the ctrl bit, Shift+P as 'P' with the shift bit */
    TuiKeyStroke ctrl = MK('P', 1, 0);
    tui_key_stroke_normalize(&ctrl);
    ck_assert_int_eq(ctrl.id, (uint32_t)'p');
    ck_assert_int_eq(ctrl.ctrl, 1);
    ck_assert_int_eq(ctrl.shift, 0);

    TuiKeyStroke sh = MK('P', 0, 1);
    tui_key_stroke_normalize(&sh);
    ck_assert_int_eq(sh.id, (uint32_t)'p');
    ck_assert_int_eq(sh.shift, 1);

    /* the parser normalizes specs too: "ctrl+P" equals a pressed Ctrl+P */
    int alts;
    int lens[TUI_KEYSEQ_MAX_ALT];
    TuiKeyStroke s[TUI_KEYSEQ_MAX_ALT][TUI_KEYSEQ_MAX_KEYS];
    ck_assert_int_eq(tui_key_parse("ctrl+P", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, (uint32_t)'p');
    ck_assert_int_eq(s[0][0].ctrl, 1);
    ck_assert_int_eq(s[0][0].shift, 0);

    /* a bare uppercase spec records shift */
    ck_assert_int_eq(tui_key_parse("P", &alts, lens, s), 0);
    ck_assert_int_eq(s[0][0].id, (uint32_t)'p');
    ck_assert_int_eq(s[0][0].shift, 1);

    /* non-letter ids are untouched */
    TuiKeyStroke f = MK(TUI_KEYID_F2, 0, 0);
    tui_key_stroke_normalize(&f);
    ck_assert_int_eq(f.id, TUI_KEYID_F2);
}
END_TEST

START_TEST(test_dispatch_ctrl_letter_matches_pressed_glyph)
{
    TuiKeymap *km = tui_keymap_create();
    ck_assert_ptr_nonnull(km);
    register_defaults(km); /* menu.open = "ctrl+m,ctrl+p" */

    const char *name = NULL;
    /* notcurses reports Ctrl+P as id 'P' (uppercase) with the ctrl bit */
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('P', 1, 0), 0, &name), TUI_KEYMAP_CMD);
    ck_assert_str_eq(name, "menu.open");

    /* leader "ctrl+x" matches the pressed Ctrl+X ('X' + ctrl) */
    ck_assert_int_eq(tui_keymap_dispatch(km, &MK('X', 1, 0), 0, &name), TUI_KEYMAP_LEADER);
    tui_keymap_leader_clear(km);
    tui_keymap_destroy(km);
}
END_TEST

static Suite *tui_keys_suite(void)
{
    Suite *s = suite_create("tui_keys");
    TCase *tc = tcase_create("tui_keys");
    tcase_add_test(tc, test_parse_single_char);
    tcase_add_test(tc, test_parse_modifiers);
    tcase_add_test(tc, test_parse_key_names);
    tcase_add_test(tc, test_parse_alternatives_and_chord);
    tcase_add_test(tc, test_parse_errors);
    tcase_add_test(tc, test_stroke_equality);
    tcase_add_test(tc, test_stroke_to_string);
    tcase_add_test(tc, test_dispatch_single_keys);
    tcase_add_test(tc, test_leader_arms_and_resolves);
    tcase_add_test(tc, test_leader_timeout);
    tcase_add_test(tc, test_leader_miss_falls_through);
    tcase_add_test(tc, test_bind_override_and_disable);
    tcase_add_test(tc, test_bad_registration_spec_fails);
    tcase_add_test(tc, test_register_keeps_override_state);
    tcase_add_test(tc, test_walk_and_lookup);
    tcase_add_test(tc, test_custom_leader);
    tcase_add_test(tc, test_count_and_at);
    tcase_add_test(tc, test_normalize_case);
    tcase_add_test(tc, test_dispatch_ctrl_letter_matches_pressed_glyph);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(tui_keys_suite());
    srunner_set_fork_status(sr, CK_FORK);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
