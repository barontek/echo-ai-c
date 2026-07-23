#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "utils/string_utils.h"

START_TEST(test_str_trim)
{
    char s1[] = "  hello  ";
    ck_assert_str_eq(str_trim(s1), "hello");

    char s2[] = "no trim";
    ck_assert_str_eq(str_trim(s2), "no trim");

    char s3[] = "";
    ck_assert_str_eq(str_trim(s3), "");

    char s4[] = "   ";
    ck_assert_str_eq(str_trim(s4), "");
}
END_TEST

START_TEST(test_str_starts_ends)
{
    ck_assert_int_eq(str_starts_with("hello world", "hello"), 1);
    ck_assert_int_eq(str_starts_with("hello world", "world"), 0);
    ck_assert_int_eq(str_starts_with("", ""), 1);

    ck_assert_int_eq(str_ends_with("hello.c", ".c"), 1);
    ck_assert_int_eq(str_ends_with("hello.c", ".h"), 0);
}
END_TEST

START_TEST(test_str_split)
{
    StrArray arr = str_split("a,b,c", ',');
    ck_assert_int_eq(arr.count, 3);
    ck_assert_str_eq(arr.items[0], "a");
    ck_assert_str_eq(arr.items[1], "b");
    ck_assert_str_eq(arr.items[2], "c");
    str_array_free(&arr);

    arr = str_split("", ',');
    ck_assert_int_eq(arr.count, 1);
    ck_assert_str_eq(arr.items[0], "");
    str_array_free(&arr);
}
END_TEST

Suite *string_suite(void)
{
    Suite *s = suite_create("StringUtils");
    TCase *tc = tcase_create("Core");
    tcase_add_test(tc, test_str_trim);
    tcase_add_test(tc, test_str_starts_ends);
    tcase_add_test(tc, test_str_split);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = string_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
