/*
 * test_search_provider.c - unit tests for the search-provider factory:
 * name resolution and error cases. Depends on: check,
 * search_provider.h.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "tools/search_provider.h"

START_TEST(test_search_provider_create_known_backends)
{
    const char *names[] = {"brave", "duckduckgo", "tavily"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        SearchProvider *sp = search_provider_create(names[i], "key");
        ck_assert_ptr_nonnull(sp);
        ck_assert_str_eq(sp->name, names[i]);
        ck_assert(sp->search != NULL);
        ck_assert(sp->destroy != NULL);
        sp->destroy(sp);
    }
}
END_TEST

START_TEST(test_search_provider_create_unknown_name_returns_null)
{
    SearchProvider *sp = search_provider_create("not_a_backend", "key");
    ck_assert_ptr_null(sp);
}
END_TEST

START_TEST(test_search_provider_create_null_name_returns_null)
{
    SearchProvider *sp = search_provider_create(NULL, "key");
    ck_assert_ptr_null(sp);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("SearchProvider");
    TCase *tc = tcase_create("Create");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_search_provider_create_known_backends);
    tcase_add_test(tc, test_search_provider_create_unknown_name_returns_null);
    tcase_add_test(tc, test_search_provider_create_null_name_returns_null);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
