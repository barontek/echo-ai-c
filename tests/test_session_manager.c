#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "session/session_manager.h"
#include "session/encryption.h"

START_TEST(test_session_list_alloc_fail_mid)
{
    char tmpdir[] = "/tmp/test_sm_XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(tmpdir));

    SessionManager *sm = session_manager_create(tmpdir, "test_password");
    ck_assert_ptr_nonnull(sm);

    Session *s1 = session_manager_create_session(sm, "session one");
    ck_assert_ptr_nonnull(s1);
    session_free(s1);

    Session *s2 = session_manager_create_session(sm, "session two");
    ck_assert_ptr_nonnull(s2);
    session_free(s2);

    session_manager_test_set_alloc_fail(4);
    SessionList *list = session_manager_list_sessions(sm);
    ck_assert_ptr_nonnull(list);
    ck_assert_int_eq(list->count, 1);
    ck_assert_str_eq(list->titles[0], "session one");
    session_list_free(list);

    session_manager_test_set_alloc_fail(-1);
    list = session_manager_list_sessions(sm);
    ck_assert_ptr_nonnull(list);
    ck_assert_int_eq(list->count, 2);
    session_list_free(list);

    session_manager_free(sm);

    char rm[4096];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int rc = system(rm);
    (void)rc;
}
END_TEST

Suite *session_mgr_suite(void)
{
    Suite *s = suite_create("SessionManager");
    TCase *tc = tcase_create("Core");
    tcase_add_test(tc, test_session_list_alloc_fail_mid);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = session_mgr_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}