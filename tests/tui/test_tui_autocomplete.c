/*
 * test_tui_autocomplete.c - slash-command completion: single-match,
 * alias match, longest-common-prefix across several matches, no-match,
 * prefix not extended. Depends on: check.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tui/tui_autocomplete.h"

static TuiCommandRegistry *make_registry(void)
{
    TuiCommandRegistry *r = tui_command_registry_create();
    ck_assert_ptr_nonnull(r);
    TuiCommand c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "session.list");
    snprintf(c.title, sizeof(c.title), "Sessions");
    snprintf(c.slash, sizeof(c.slash), "sessions");
    snprintf(c.aliases, sizeof(c.aliases), "resume, continue");
    ck_assert_int_eq(tui_command_register(r, &c), 0);
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "session.save");
    snprintf(c.title, sizeof(c.title), "Save");
    snprintf(c.slash, sizeof(c.slash), "save");
    ck_assert_int_eq(tui_command_register(r, &c), 0);
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "session.new");
    snprintf(c.title, sizeof(c.title), "New");
    snprintf(c.slash, sizeof(c.slash), "new");
    ck_assert_int_eq(tui_command_register(r, &c), 0);
    return r;
}

START_TEST(test_slash_completes_unique_match)
{
    TuiCommandRegistry *r = make_registry();
    char out[64];
    ck_assert_int_eq(tui_autocomplete_slash(r, "/sav", out, sizeof(out)), 1);
    ck_assert_str_eq(out, "/save ");
    /* the whole input is replaced, not just the prefix */
    ck_assert_int_eq(tui_autocomplete_slash(r, "/new", out, sizeof(out)), 1);
    ck_assert_str_eq(out, "/new ");
    tui_command_registry_destroy(r);
}
END_TEST

START_TEST(test_slash_completes_via_alias)
{
    TuiCommandRegistry *r = make_registry();
    char out[64];
    ck_assert_int_eq(tui_autocomplete_slash(r, "/resu", out, sizeof(out)), 1);
    ck_assert_str_eq(out, "/resume ");
    tui_command_registry_destroy(r);
}
END_TEST

START_TEST(test_slash_longest_common_prefix)
{
    TuiCommandRegistry *r = make_registry();
    char out[64];
    /* "/s" matches sessions + save whose common prefix is just "s" (no
     * extension), so nothing is written */
    ck_assert_int_eq(tui_autocomplete_slash(r, "/s", out, sizeof(out)), 0);
    /* "/se" matches only sessions -> unique full completion */
    ck_assert_int_eq(tui_autocomplete_slash(r, "/se", out, sizeof(out)), 1);
    ck_assert_str_eq(out, "/sessions ");
    tui_command_registry_destroy(r);
}
END_TEST

START_TEST(test_slash_lcp_extends_input)
{
    /* "search" and "secret" share the "se" prefix: "/s" -> "/se" */
    TuiCommandRegistry *r = tui_command_registry_create();
    ck_assert_ptr_nonnull(r);
    TuiCommand c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "search");
    snprintf(c.title, sizeof(c.title), "Search");
    snprintf(c.slash, sizeof(c.slash), "search");
    ck_assert_int_eq(tui_command_register(r, &c), 0);
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "secret");
    snprintf(c.title, sizeof(c.title), "Secret");
    snprintf(c.slash, sizeof(c.slash), "secret");
    ck_assert_int_eq(tui_command_register(r, &c), 0);

    char out[64];
    ck_assert_int_eq(tui_autocomplete_slash(r, "/s", out, sizeof(out)), 1);
    ck_assert_str_eq(out, "/se");
    tui_command_registry_destroy(r);
}
END_TEST

START_TEST(test_slash_no_match)
{
    TuiCommandRegistry *r = make_registry();
    char out[64];
    ck_assert_int_eq(tui_autocomplete_slash(r, "/zzz", out, sizeof(out)), 0);
    ck_assert_int_eq(tui_autocomplete_slash(r, "/", out, sizeof(out)), 0);
    ck_assert_int_eq(tui_autocomplete_slash(r, "not a slash", out, sizeof(out)), 0);
    ck_assert_int_eq(tui_autocomplete_slash(r, NULL, out, sizeof(out)), 0);
    tui_command_registry_destroy(r);
}
END_TEST

static Suite *tui_autocomplete_suite(void)
{
    Suite *s = suite_create("tui_autocomplete");
    TCase *tc = tcase_create("autocomplete");
    tcase_add_test(tc, test_slash_completes_unique_match);
    tcase_add_test(tc, test_slash_completes_via_alias);
    tcase_add_test(tc, test_slash_longest_common_prefix);
    tcase_add_test(tc, test_slash_lcp_extends_input);
    tcase_add_test(tc, test_slash_no_match);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(tui_autocomplete_suite());
    srunner_set_fork_status(sr, CK_FORK);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}