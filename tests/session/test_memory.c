#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "session/memory.h"

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
    MemoryFact *facts = memory_list_all(db, &count);
    ck_assert_ptr_null(facts);
    ck_assert_int_eq(count, 0);

    memory_test_set_alloc_fail(-1);
    count = 0;
    facts = memory_list_all(db, &count);
    ck_assert_ptr_nonnull(facts);
    ck_assert_int_eq(count, 2);
    ck_assert_str_eq(facts[0].key, "key1");
    ck_assert_str_eq(facts[0].value, "val1");
    ck_assert_str_eq(facts[1].key, "key2");
    ck_assert_str_eq(facts[1].value, "val2");
    memory_facts_free(facts, count);

    sqlite3_close(db);
}
END_TEST

Suite *memory_suite(void)
{
    Suite *s = suite_create("Memory");
    TCase *tc = tcase_create("Core");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_memory_list_alloc_fail_mid);
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
