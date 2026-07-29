#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "utils/json.h"
#include "utils/string_utils.h"

START_TEST(test_json_string_escape_quotes)
{
    char *esc = json_string_escape("hello \"world\"");
    ck_assert_ptr_nonnull(esc);
    ck_assert_str_eq(esc, "hello \\\"world\\\"");
    free(esc);
}
END_TEST

START_TEST(test_json_string_escape_backslash)
{
    char *esc = json_string_escape("path\\to\\file");
    ck_assert_ptr_nonnull(esc);
    ck_assert_str_eq(esc, "path\\\\to\\\\file");
    free(esc);
}
END_TEST

START_TEST(test_json_string_escape_newline_tab_carriage)
{
    char *esc = json_string_escape("line1\nline2\tindented\r");
    ck_assert_ptr_nonnull(esc);
    ck_assert_str_eq(esc, "line1\\nline2\\tindented\\r");
    free(esc);
}
END_TEST

START_TEST(test_json_string_escape_no_special_chars)
{
    char *esc = json_string_escape("plain text");
    ck_assert_ptr_nonnull(esc);
    ck_assert_str_eq(esc, "plain text");
    free(esc);
}
END_TEST

START_TEST(test_json_string_escape_null)
{
    char *esc = json_string_escape(NULL);
    ck_assert_ptr_null(esc);
}
END_TEST

START_TEST(test_json_string_escape_empty)
{
    char *esc = json_string_escape("");
    ck_assert_ptr_nonnull(esc);
    ck_assert_str_eq(esc, "");
    free(esc);
}
END_TEST

START_TEST(test_json_string_escape_mixed)
{
    char *esc = json_string_escape("a\"b\\c\nd\te\rf");
    ck_assert_ptr_nonnull(esc);
    ck_assert_str_eq(esc, "a\\\"b\\\\c\\nd\\te\\rf");
    free(esc);
}
END_TEST

START_TEST(test_json_add_string_null_val)
{
    cJSON *obj = cJSON_CreateObject();
    ck_assert_ptr_nonnull(obj);
    cJSON *added = json_add_string(obj, "key", NULL);
    ck_assert_ptr_nonnull(added);
    ck_assert_str_eq(cJSON_GetObjectItem(obj, "key")->valuestring, "");
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_json_add_string_nonnull_val)
{
    cJSON *obj = cJSON_CreateObject();
    json_add_string(obj, "key", "value");
    ck_assert_str_eq(cJSON_GetObjectItem(obj, "key")->valuestring, "value");
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_json_add_int)
{
    cJSON *obj = cJSON_CreateObject();
    json_add_int(obj, "count", 42);
    ck_assert_int_eq(cJSON_GetObjectItem(obj, "count")->valueint, 42);
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_json_add_double)
{
    cJSON *obj = cJSON_CreateObject();
    json_add_double(obj, "pi", 3.14);
    ck_assert(cJSON_GetObjectItem(obj, "pi")->valuedouble > 3.13);
    ck_assert(cJSON_GetObjectItem(obj, "pi")->valuedouble < 3.15);
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_json_serialize_object)
{
    cJSON *obj = cJSON_CreateObject();
    json_add_string(obj, "name", "test");
    json_add_int(obj, "value", 100);
    char *s = json_serialize(obj);
    ck_assert_ptr_nonnull(s);
    ck_assert_str_eq(s, "{\"name\":\"test\",\"value\":100}");
    free(s);
    cJSON_Delete(obj);
}
END_TEST

START_TEST(test_json_serialize_nested)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *child = cJSON_CreateObject();
    json_add_string(child, "inner", "x");
    cJSON_AddItemToObject(obj, "outer", child);
    char *s = json_serialize(obj);
    ck_assert_ptr_nonnull(s);
    ck_assert_str_eq(s, "{\"outer\":{\"inner\":\"x\"}}");
    free(s);
    cJSON_Delete(obj);
}
END_TEST

Suite *json_suite(void)
{
    Suite *s = suite_create("JSON");
    TCase *tc = tcase_create("Escape");
    tcase_set_timeout(tc, 10);
    tcase_add_test(tc, test_json_string_escape_quotes);
    tcase_add_test(tc, test_json_string_escape_backslash);
    tcase_add_test(tc, test_json_string_escape_newline_tab_carriage);
    tcase_add_test(tc, test_json_string_escape_no_special_chars);
    tcase_add_test(tc, test_json_string_escape_null);
    tcase_add_test(tc, test_json_string_escape_empty);
    tcase_add_test(tc, test_json_string_escape_mixed);
    suite_add_tcase(s, tc);

    TCase *tc2 = tcase_create("Add");
    tcase_set_timeout(tc2, 10);
    tcase_add_test(tc2, test_json_add_string_null_val);
    tcase_add_test(tc2, test_json_add_string_nonnull_val);
    tcase_add_test(tc2, test_json_add_int);
    tcase_add_test(tc2, test_json_add_double);
    suite_add_tcase(s, tc2);

    TCase *tc3 = tcase_create("Serialize");
    tcase_set_timeout(tc3, 10);
    tcase_add_test(tc3, test_json_serialize_object);
    tcase_add_test(tc3, test_json_serialize_nested);
    suite_add_tcase(s, tc3);

    return s;
}

int main(void)
{
    Suite *s = json_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
