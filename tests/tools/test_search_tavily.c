/*
 * test_search_tavily.c - unit tests for the Tavily search response
 * parser with canned JSON (no network). Depends on: check,
 * search_provider.h.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include "tools/search_provider.h"

char *search_tavily_test_parse_response(const char *raw);

START_TEST(test_tavily_parse_maps_results_to_shared_shape)
{
    char *out = search_tavily_test_parse_response(
        "{\"results\":["
        "{\"title\":\"One\",\"url\":\"https://a\",\"content\":\"c1\"},"
        "{\"title\":\"Two\",\"url\":\"https://b\",\"content\":\"c2\"}"
        "]}");
    ck_assert_ptr_nonnull(out);
    cJSON *arr = cJSON_Parse(out);
    ck_assert_ptr_nonnull(arr);
    ck_assert_int_eq(cJSON_GetArraySize(arr), 2);
    cJSON *first = cJSON_GetArrayItem(arr, 0);
    ck_assert_str_eq(cJSON_GetObjectItem(first, "title")->valuestring, "One");
    ck_assert_str_eq(cJSON_GetObjectItem(first, "snippet")->valuestring, "c1");
    cJSON_Delete(arr);
    free(out);
}
END_TEST

START_TEST(test_tavily_parse_invalid_json_returns_error_string)
{
    char *out = search_tavily_test_parse_response("garbage");
    ck_assert_ptr_nonnull(out);
    ck_assert(strstr(out, "Error:") != NULL);
    free(out);
}
END_TEST

START_TEST(test_tavily_parse_missing_results_is_empty_array)
{
    char *out = search_tavily_test_parse_response("{\"query\":\"x\"}");
    ck_assert_ptr_nonnull(out);
    cJSON *arr = cJSON_Parse(out);
    ck_assert_ptr_nonnull(arr);
    ck_assert_int_eq(cJSON_GetArraySize(arr), 0);
    cJSON_Delete(arr);
    free(out);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("SearchTavily");
    TCase *tc = tcase_create("Parse");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_tavily_parse_maps_results_to_shared_shape);
    tcase_add_test(tc, test_tavily_parse_invalid_json_returns_error_string);
    tcase_add_test(tc, test_tavily_parse_missing_results_is_empty_array);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
