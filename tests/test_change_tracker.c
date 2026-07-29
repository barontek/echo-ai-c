#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "change_tracker/change_tracker.h"

START_TEST(test_ct_create_initializes_empty_undo_redo_stacks)
{
    ChangeTracker *ct = ct_create();
    ck_assert_ptr_nonnull(ct);
    ck_assert_int_eq(ct->undo_count, 0);
    ck_assert_int_eq(ct->redo_count, 0);
    ct_destroy(ct);
}
END_TEST

START_TEST(test_ct_snapshot_no_file)
{
    ChangeTracker *ct = ct_create();
    ck_assert_int_eq(ct_snapshot(ct, "/nonexistent/file"), -1);
    ck_assert_int_eq(ct->undo_count, 0);
    ct_destroy(ct);
}
END_TEST

START_TEST(test_ct_undo_restores_original_content_and_redo_reapplies_changes)
{
    ChangeTracker *ct = ct_create();

    FILE *f = fopen("/tmp/ct_test.txt", "w");
    ck_assert_ptr_nonnull(f);
    fputs("original", f);
    fclose(f);

    ck_assert_int_eq(ct_snapshot(ct, "/tmp/ct_test.txt"), 0);
    ck_assert_int_eq(ct->undo_count, 1);

    f = fopen("/tmp/ct_test.txt", "w");
    fputs("modified", f);
    fclose(f);

    ck_assert_int_gt(ct_undo(ct), 0);
    ck_assert_int_eq(ct->undo_count, 0);
    ck_assert_int_eq(ct->redo_count, 1);

    char buf[64] = {0};
    f = fopen("/tmp/ct_test.txt", "r");
    ck_assert_ptr_nonnull(f);
    ck_assert_ptr_nonnull(fgets(buf, sizeof(buf), f));
    fclose(f);
    ck_assert_str_eq(buf, "original");

    ck_assert_int_gt(ct_redo(ct), 0);
    ck_assert_int_eq(ct->redo_count, 0);
    ck_assert_int_eq(ct->undo_count, 1);

    f = fopen("/tmp/ct_test.txt", "r");
    ck_assert_ptr_nonnull(f);
    ck_assert_ptr_nonnull(fgets(buf, sizeof(buf), f));
    fclose(f);
    ck_assert_str_eq(buf, "modified");

    remove("/tmp/ct_test.txt");
    ct_destroy(ct);
}
END_TEST

START_TEST(test_ct_snapshot_alloc_fail_mid)
{
    ChangeTracker *ct = ct_create();

    FILE *f = fopen("/tmp/ct_fail.txt", "w");
    ck_assert_ptr_nonnull(f);
    fputs("content", f);
    fclose(f);

    change_tracker_test_set_alloc_fail(1);
    ck_assert_int_eq(ct_snapshot(ct, "/tmp/ct_fail.txt"), -1);
    ck_assert_int_eq(ct->undo_count, 0);

    change_tracker_test_set_alloc_fail(-1);
    ck_assert_int_eq(ct_snapshot(ct, "/tmp/ct_fail.txt"), 0);
    ck_assert_int_eq(ct->undo_count, 1);

    remove("/tmp/ct_fail.txt");
    ct_destroy(ct);
}
END_TEST

START_TEST(test_ct_undo_alloc_fail_mid)
{
    ChangeTracker *ct = ct_create();

    FILE *f = fopen("/tmp/ct_undo_fail.txt", "w");
    ck_assert_ptr_nonnull(f);
    fputs("original", f);
    fclose(f);

    ck_assert_int_eq(ct_snapshot(ct, "/tmp/ct_undo_fail.txt"), 0);
    ck_assert_int_eq(ct->undo_count, 1);

    f = fopen("/tmp/ct_undo_fail.txt", "w");
    fputs("modified", f);
    fclose(f);

    change_tracker_test_set_alloc_fail(1);
    ck_assert_int_eq(ct_undo(ct), -1);
    ck_assert_int_eq(ct->undo_count, 1);
    ck_assert_int_eq(ct->redo_count, 0);

    change_tracker_test_set_alloc_fail(-1);
    ck_assert_int_gt(ct_undo(ct), 0);
    ck_assert_int_eq(ct->undo_count, 0);
    ck_assert_int_eq(ct->redo_count, 1);

    remove("/tmp/ct_undo_fail.txt");
    ct_destroy(ct);
}
END_TEST

Suite *change_tracker_suite(void)
{
    Suite *s = suite_create("ChangeTracker");

    TCase *tc_lifecycle = tcase_create("Lifecycle");
    tcase_set_timeout(tc_lifecycle, 30);
    tcase_add_test(tc_lifecycle, test_ct_create_initializes_empty_undo_redo_stacks);
    tcase_add_test(tc_lifecycle, test_ct_snapshot_no_file);
    tcase_add_test(tc_lifecycle, test_ct_undo_restores_original_content_and_redo_reapplies_changes);
    suite_add_tcase(s, tc_lifecycle);

    TCase *tc_fault = tcase_create("FaultInjection");
    tcase_set_timeout(tc_fault, 30);
    tcase_add_test(tc_fault, test_ct_snapshot_alloc_fail_mid);
    tcase_add_test(tc_fault, test_ct_undo_alloc_fail_mid);
    suite_add_tcase(s, tc_fault);

    return s;
}

int main(void)
{
    Suite *s = change_tracker_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}