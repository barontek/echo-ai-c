/*
 * test_tui_command.c - command registry: registration, canonical-name
 * lookup, slash-name + alias resolution, walk/count/at, replace-on-
 * re-register. Depends on: check.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tui/tui_command.h"

static int handler_calls = 0;
static char last_args[64];

static void handler(void *ud, const char *args)
{
    (void)ud;
    handler_calls++;
    snprintf(last_args, sizeof(last_args), "%s", args ? args : "");
}

static void reg(TuiCommandRegistry *r, const char *name, const char *title,
                const char *cat, const char *slash, const char *aliases)
{
    TuiCommand c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "%s", name);
    snprintf(c.title, sizeof(c.title), "%s", title);
    snprintf(c.desc, sizeof(c.desc), "desc for %s", name);
    snprintf(c.category, sizeof(c.category), "%s", cat);
    snprintf(c.slash, sizeof(c.slash), "%s", slash);
    snprintf(c.aliases, sizeof(c.aliases), "%s", aliases);
    c.fn = handler;
    c.suggested = strcmp(name, "session.new") == 0;
    ck_assert_int_eq(tui_command_register(r, &c), 0);
}

START_TEST(test_register_and_lookup)
{
    TuiCommandRegistry *r = tui_command_registry_create();
    ck_assert_ptr_nonnull(r);
    ck_assert_int_eq(tui_command_count(r), 0);

    reg(r, "session.new", "New session", "Session", "new", "");
    reg(r, "session.list", "Sessions", "Session", "sessions", "resume, continue");
    ck_assert_int_eq(tui_command_count(r), 2);

    const TuiCommand *c = tui_command_find(r, "session.list");
    ck_assert_ptr_nonnull(c);
    ck_assert_str_eq(c->title, "Sessions");
    ck_assert_ptr_null(tui_command_find(r, "nope"));

    c = tui_command_at(r, 0);
    ck_assert_ptr_nonnull(c);
    ck_assert_str_eq(c->name, "session.new");
    ck_assert_ptr_null(tui_command_at(r, 2));
    tui_command_registry_destroy(r);
}
END_TEST

START_TEST(test_slash_resolution_and_aliases)
{
    TuiCommandRegistry *r = tui_command_registry_create();
    ck_assert_ptr_nonnull(r);
    reg(r, "session.list", "Sessions", "Session", "sessions", "resume, continue");
    reg(r, "app.exit", "Quit", "App", "exit", "quit, q");

    ck_assert_ptr_eq(tui_command_find_slash(r, "sessions"),
                     tui_command_find(r, "session.list"));
    ck_assert_ptr_eq(tui_command_find_slash(r, "resume"),
                     tui_command_find(r, "session.list"));
    ck_assert_ptr_eq(tui_command_find_slash(r, "continue"),
                     tui_command_find(r, "session.list"));
    ck_assert_ptr_eq(tui_command_find_slash(r, "q"),
                     tui_command_find(r, "app.exit"));
    ck_assert_ptr_null(tui_command_find_slash(r, "sess"));
    ck_assert_ptr_null(tui_command_find_slash(r, "SESSIONS")); /* case-sensitive */

    ck_assert_int_eq(tui_command_slash_matches(tui_command_find(r, "session.list"),
                                               "resume"), 1);
    ck_assert_int_eq(tui_command_slash_matches(tui_command_find(r, "session.list"),
                                               "nope"), 0);
    tui_command_registry_destroy(r);
}
END_TEST

START_TEST(test_handler_invocation_through_lookup)
{
    TuiCommandRegistry *r = tui_command_registry_create();
    ck_assert_ptr_nonnull(r);
    reg(r, "session.load", "Load session", "Session", "load", "");

    const TuiCommand *c = tui_command_find(r, "session.load");
    /* (function pointers cannot be asserted nonnull under -Wpedantic;
     * invoking a NULL fn would crash the fork, so the call is the check) */
    handler_calls = 0;
    c->fn(NULL, "s123");
    ck_assert_int_eq(handler_calls, 1);
    ck_assert_str_eq(last_args, "s123");

    c->fn(NULL, NULL);
    ck_assert_int_eq(handler_calls, 2);
    ck_assert_str_eq(last_args, "");
    tui_command_registry_destroy(r);
}
END_TEST

START_TEST(test_re_register_replaces_but_keeps_order)
{
    TuiCommandRegistry *r = tui_command_registry_create();
    ck_assert_ptr_nonnull(r);
    reg(r, "a", "A", "Cat", "a", "");
    reg(r, "b", "B", "Cat", "b", "");
    reg(r, "a", "A2", "Cat2", "a2", "");

    ck_assert_int_eq(tui_command_count(r), 2);
    const TuiCommand *a = tui_command_at(r, 0);
    ck_assert_str_eq(a->name, "a");
    ck_assert_str_eq(a->title, "A2");
    ck_assert_str_eq(a->slash, "a2");
    ck_assert_ptr_null(tui_command_find_slash(r, "a"));
    ck_assert_ptr_eq(tui_command_find_slash(r, "a2"), a);
    tui_command_registry_destroy(r);
}
END_TEST

struct walk_ud { int count; const char *first; };

static void walk_cb(void *ud, const TuiCommand *c)
{
    struct walk_ud *w = ud;
    if (w->count == 0) w->first = c->name;
    w->count++;
}

START_TEST(test_walk_and_suggested_flag)
{
    TuiCommandRegistry *r = tui_command_registry_create();
    ck_assert_ptr_nonnull(r);
    reg(r, "session.new", "New session", "Session", "new", "");
    reg(r, "session.list", "Sessions", "Session", "sessions", "");
    reg(r, "app.exit", "Quit", "App", "exit", "");

    struct walk_ud w = { 0, NULL };
    tui_command_walk(r, walk_cb, &w);
    ck_assert_int_eq(w.count, 3);
    ck_assert_str_eq(w.first, "session.new");

    ck_assert_int_eq(tui_command_find(r, "session.new")->suggested, 1);
    ck_assert_int_eq(tui_command_find(r, "app.exit")->suggested, 0);
    tui_command_registry_destroy(r);
}
END_TEST

static Suite *tui_command_suite(void)
{
    Suite *s = suite_create("tui_command");
    TCase *tc = tcase_create("registry");
    tcase_add_test(tc, test_register_and_lookup);
    tcase_add_test(tc, test_slash_resolution_and_aliases);
    tcase_add_test(tc, test_handler_invocation_through_lookup);
    tcase_add_test(tc, test_re_register_replaces_but_keeps_order);
    tcase_add_test(tc, test_walk_and_suggested_flag);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(tui_command_suite());
    srunner_set_fork_status(sr, CK_FORK);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}