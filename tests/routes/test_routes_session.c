#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>

#include "../src/server/routes/routes_session.h"

extern const char *session_id_from_path(const char *path);
extern int is_export_path(const char *sid);
extern size_t export_suffix_len(const char *sid);

/* ---------------------------------------------------------------------------
 * session_id_from_path
 * --------------------------------------------------------------------------- */

START_TEST(test_session_id_from_path_valid)
{
    const char *sid = session_id_from_path("/api/sessions/abc-123");
    ck_assert_ptr_nonnull(sid);
    ck_assert_str_eq(sid, "abc-123");
}
END_TEST

START_TEST(test_session_id_from_path_exact_prefix_no_id)
{
    const char *sid = session_id_from_path("/api/sessions/");
    ck_assert_ptr_null(sid);
}
END_TEST

START_TEST(test_session_id_from_path_different_path)
{
    const char *sid = session_id_from_path("/api/status");
    ck_assert_ptr_null(sid);
}
END_TEST

START_TEST(test_session_id_from_path_empty)
{
    const char *sid = session_id_from_path("");
    ck_assert_ptr_null(sid);
}
END_TEST

START_TEST(test_session_id_from_path_short_path)
{
    const char *sid = session_id_from_path("/api");
    ck_assert_ptr_null(sid);
}
END_TEST

START_TEST(test_session_id_from_path_with_export_suffix)
{
    /* session_id_from_path doesn't strip /export — that's done later */
    const char *sid = session_id_from_path("/api/sessions/abc/export");
    ck_assert_ptr_nonnull(sid);
    ck_assert_str_eq(sid, "abc/export");
}
END_TEST

START_TEST(test_session_id_from_path_uuid)
{
    const char *sid = session_id_from_path(
        "/api/sessions/550e8400-e29b-41d4-a716-446655440000");
    ck_assert_ptr_nonnull(sid);
    ck_assert_str_eq(sid, "550e8400-e29b-41d4-a716-446655440000");
}
END_TEST

/* ---------------------------------------------------------------------------
 * is_export_path
 * --------------------------------------------------------------------------- */

START_TEST(test_is_export_path_export_suffix)
{
    ck_assert_int_eq(is_export_path("session-id/export"), 1);
}
END_TEST

START_TEST(test_is_export_path_debug_export_suffix)
{
    ck_assert_int_eq(is_export_path("session-id/debug-export"), 1);
}
END_TEST

START_TEST(test_is_export_path_no_suffix)
{
    ck_assert_int_eq(is_export_path("session-id"), 0);
}
END_TEST

START_TEST(test_is_export_path_just_export)
{
    /* strlen("/export") == 7, slen > 7 needed, so false */
    ck_assert_int_eq(is_export_path("/export"), 0);
}
END_TEST

START_TEST(test_is_export_path_just_debug_export)
{
    /* strlen("/debug-export") == 13, slen > 13 needed, so false */
    ck_assert_int_eq(is_export_path("/debug-export"), 0);
}
END_TEST

START_TEST(test_is_export_path_empty)
{
    ck_assert_int_eq(is_export_path(""), 0);
}
END_TEST

/* ---------------------------------------------------------------------------
 * export_suffix_len
 * --------------------------------------------------------------------------- */

START_TEST(test_export_suffix_len_export)
{
    ck_assert_int_eq(export_suffix_len("abc/export"), 7);
}
END_TEST

START_TEST(test_export_suffix_len_debug_export)
{
    ck_assert_int_eq(export_suffix_len("abc/debug-export"), 13);
}
END_TEST

START_TEST(test_export_suffix_len_no_suffix)
{
    ck_assert_int_eq(export_suffix_len("abc"), 0);
}
END_TEST

START_TEST(test_export_suffix_len_empty)
{
    ck_assert_int_eq(export_suffix_len(""), 0);
}
END_TEST

/* ---------------------------------------------------------------------------
 * Suite
 * --------------------------------------------------------------------------- */

Suite *routes_session_suite(void)
{
    Suite *s = suite_create("routes_session");

    TCase *tc_path = tcase_create("session_id_from_path");
    tcase_add_test(tc_path, test_session_id_from_path_valid);
    tcase_add_test(tc_path, test_session_id_from_path_exact_prefix_no_id);
    tcase_add_test(tc_path, test_session_id_from_path_different_path);
    tcase_add_test(tc_path, test_session_id_from_path_empty);
    tcase_add_test(tc_path, test_session_id_from_path_short_path);
    tcase_add_test(tc_path, test_session_id_from_path_with_export_suffix);
    tcase_add_test(tc_path, test_session_id_from_path_uuid);
    suite_add_tcase(s, tc_path);

    TCase *tc_exp = tcase_create("is_export_path");
    tcase_add_test(tc_exp, test_is_export_path_export_suffix);
    tcase_add_test(tc_exp, test_is_export_path_debug_export_suffix);
    tcase_add_test(tc_exp, test_is_export_path_no_suffix);
    tcase_add_test(tc_exp, test_is_export_path_just_export);
    tcase_add_test(tc_exp, test_is_export_path_just_debug_export);
    tcase_add_test(tc_exp, test_is_export_path_empty);
    suite_add_tcase(s, tc_exp);

    TCase *tc_suf = tcase_create("export_suffix_len");
    tcase_add_test(tc_suf, test_export_suffix_len_export);
    tcase_add_test(tc_suf, test_export_suffix_len_debug_export);
    tcase_add_test(tc_suf, test_export_suffix_len_no_suffix);
    tcase_add_test(tc_suf, test_export_suffix_len_empty);
    suite_add_tcase(s, tc_suf);

    return s;
}

int main(void)
{
    int failures = 0;
    Suite *s = routes_session_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_ENV);
    failures = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failures;
}
