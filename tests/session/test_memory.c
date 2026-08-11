#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "session/memory.h"

/* test_memory - unit tests for memory. Depends on: check, the module under test. */
START_TEST(test_memory_list_alloc_fail_mid)
{
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    ck_assert_int_eq(rc, SQLITE_OK);

    rc = memory_table_init(db);
    ck_assert_int_eq(rc, 0);

    rc = memory_set(db, "key1", "val1");
    ck_assert_int_eq(rc, 0);
    rc = memory_set(db, "key2", "val2");
    ck_assert_int_eq(rc, 0);

    int count = 0;
    memory_test_set_alloc_fail(2);
    MemoryFact *facts = memory_list_all(db, &count, NULL);
    ck_assert_ptr_null(facts);
    ck_assert_int_eq(count, 0);

    memory_test_set_alloc_fail(-1);
    count = 0;
    facts = memory_list_all(db, &count, NULL);
    ck_assert_ptr_nonnull(facts);
    ck_assert_int_eq(count, 2);
    ck_assert_str_eq(facts[0].key, "key1");
    ck_assert_str_eq(facts[0].value, "val1");
    ck_assert_str_eq(facts[1].key, "key2");
    ck_assert_str_eq(facts[1].value, "val2");
    memory_facts_free(facts, count);

    sqlite3_close(db);
}

/* E4: memory_get_dup's internal str_dup failure must return NULL with no
 * partial state, and must be distinguishable from "key absent" via the
 * is_error out-param (a store error is NOT a not-found). */
START_TEST(test_memory_get_dup_alloc_fail_and_error_distinction)
{
    sqlite3 *db = NULL;
    ck_assert_int_eq(sqlite3_open(":memory:", &db), SQLITE_OK);
    ck_assert_int_eq(memory_table_init(db), 0);
    ck_assert_int_eq(memory_set(db, "k", "v"), 0);

    int is_error = 0;
    /* absent key: NULL, is_error == 0 */
    char *val = memory_get_dup(db, "missing", &is_error);
    ck_assert_ptr_null(val);
    ck_assert_int_eq(is_error, 0);

    /* store error (closed db): NULL, is_error == 1 */
    sqlite3 *closed = NULL;
    ck_assert_int_eq(sqlite3_open(":memory:", &closed), SQLITE_OK);
    ck_assert_int_eq(memory_table_init(closed), 0);
    ck_assert_int_eq(sqlite3_close(closed), SQLITE_OK);
    is_error = 0;
    val = memory_get_dup(closed, "k", &is_error);
    ck_assert_ptr_null(val);
    ck_assert_int_eq(is_error, 1);

    /* allocation failure: NULL, is_error == 0 (the failure is the dup,
     * not the store) */
    memory_test_set_alloc_fail(1);
    is_error = 0;
    val = memory_get_dup(db, "k", &is_error);
    memory_test_set_alloc_fail(-1);
    ck_assert_ptr_null(val);
    ck_assert_int_eq(is_error, 0);

    /* normal op restored */
    val = memory_get_dup(db, "k", &is_error);
    ck_assert_ptr_nonnull(val);
    ck_assert_str_eq(val, "v");
    ck_assert_int_eq(is_error, 0);
    free(val);

    sqlite3_close(db);
}

/* E4: memory_get/memory_delete/set error paths return -1, and delete of a
 * missing key is not an error. */
START_TEST(test_memory_set_delete_error_paths)
{
    sqlite3 *db = NULL;
    ck_assert_int_eq(sqlite3_open(":memory:", &db), SQLITE_OK);
    ck_assert_int_eq(memory_table_init(db), 0);

    /* NULL args are -1, not crashes */
    ck_assert_int_eq(memory_set(NULL, "k", "v"), -1);
    ck_assert_int_eq(memory_set(db, NULL, "v"), -1);
    ck_assert_int_eq(memory_delete(NULL, "k"), -1);
    ck_assert_int_eq(memory_delete(db, NULL), -1);

    /* delete of a missing key succeeds (0) */
    ck_assert_int_eq(memory_delete(db, "never-set"), 0);

    /* store error surfaces as -1 */
    sqlite3 *closed = NULL;
    ck_assert_int_eq(sqlite3_open(":memory:", &closed), SQLITE_OK);
    ck_assert_int_eq(memory_table_init(closed), 0);
    ck_assert_int_eq(sqlite3_close(closed), SQLITE_OK);
    ck_assert_int_eq(memory_set(closed, "k", "v"), -1);
    ck_assert_int_eq(memory_delete(closed, "k"), -1);

    sqlite3_close(db);
}
END_TEST

Suite *memory_suite(void)
{
    Suite *s = suite_create("Memory");
    TCase *tc = tcase_create("Core");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_memory_list_alloc_fail_mid);
    tcase_add_test(tc, test_memory_get_dup_alloc_fail_and_error_distinction);
    tcase_add_test(tc, test_memory_set_delete_error_paths);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = memory_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
