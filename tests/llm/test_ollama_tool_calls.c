/*
 * test_ollama_tool_calls.c - tests for the real parse_stream_tool_calls
 * in src/llm/ollama.c (compiled here under OLLAMA_TEST via
 * ollama_test_parse_stream_calls_json). This used to be a hand-rolled
 * mirror; mirrors drift (the old one diverged on the OOM path), so the
 * tests now drive production code directly, including its silent
 * keep-old-capacity realloc behavior. Depends on: check, ollama.c,
 * message.c, string_utils, logging.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <check.h>

#include "llm/ollama.h"


static int count_calls(const char *json_str)
{
    int count = ollama_test_parse_stream_calls_json(json_str);
    ck_assert_int_ge(count, 0);
    return count;
}

START_TEST(test_parse_extracts_name_and_arguments_from_single_tool_call)
{
    ck_assert_int_eq(count_calls(
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"bash\",\"arguments\":{\"cmd\":\"ls\"}}}"
        "]}}"), 1);
}
END_TEST

START_TEST(test_parse_extracts_all_tool_calls_from_array)
{
    ck_assert_int_eq(count_calls(
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"bash\",\"arguments\":{\"cmd\":\"ls\"}}},"
        "{\"function\":{\"name\":\"read_file\",\"arguments\":{\"file\":\"x\"}}}"
        "]}}"), 2);
}
END_TEST

START_TEST(test_parse_skips_entry_when_tool_call_name_is_empty)
{
    ck_assert_int_eq(count_calls(
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"\",\"arguments\":{}}}"
        "]}}"), 0);
}
END_TEST

START_TEST(test_parse_returns_zero_when_tool_calls_field_is_missing)
{
    ck_assert_int_eq(count_calls("{\"message\":{\"content\":\"hello\"}}"), 0);
}
END_TEST

START_TEST(test_parse_invalid_json_returns_minus_one)
{
    ck_assert_int_eq(ollama_test_parse_stream_calls_json("not json"), -1);
    ck_assert_int_eq(ollama_test_parse_stream_calls_json(NULL), -1);
}
END_TEST

START_TEST(test_parse_accumulates_tool_calls_across_repeated_invocations)
{
    /* the hook owns a fresh WriteBuf per call, so repeated calls are
     * independent — the accumulation contract belongs to the streaming
     * loop (covered in test_ollama.c), not to a single JSON payload */
    ck_assert_int_eq(count_calls(
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"bash\",\"arguments\":{\"cmd\":\"ls\"}}}"
        "]}}"), 1);
    ck_assert_int_eq(count_calls(
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"bash\",\"arguments\":{\"cmd\":\"ls\"}}}"
        "]}}"), 1);
}
END_TEST

START_TEST(test_parse_grows_capacity_when_tool_calls_exceed_initial_allocation)
{
    const char *json_str =
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"a\",\"arguments\":{\"k\":\"v\"}}},"
        "{\"function\":{\"name\":\"b\",\"arguments\":{\"k\":\"v\"}}},"
        "{\"function\":{\"name\":\"c\",\"arguments\":{\"k\":\"v\"}}},"
        "{\"function\":{\"name\":\"d\",\"arguments\":{\"k\":\"v\"}}},"
        "{\"function\":{\"name\":\"e\",\"arguments\":{\"k\":\"v\"}}},"
        "{\"function\":{\"name\":\"f\",\"arguments\":{\"k\":\"v\"}}}"
        "]}}";
    ck_assert_int_eq(count_calls(json_str), 6);
}
END_TEST

/* Production semantics: when the cap-growth realloc fails, the buffer
 * keeps its OLD capacity and the overflow call is silently skipped
 * (tool_calls_count < tool_calls_cap is the guard) — the loop does NOT
 * break. The old mirror broke out of the loop instead; this asserts the
 * real behavior so the divergence cannot silently return. */
START_TEST(test_parse_realloc_failure_keeps_old_capacity_and_skips_overflow)
{
    const char *json_str =
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"a\",\"arguments\":{\"k\":\"v\"}}},"
        "{\"function\":{\"name\":\"b\",\"arguments\":{\"k\":\"v\"}}},"
        "{\"function\":{\"name\":\"c\",\"arguments\":{\"k\":\"v\"}}},"
        "{\"function\":{\"name\":\"d\",\"arguments\":{\"k\":\"v\"}}},"
        "{\"function\":{\"name\":\"e\",\"arguments\":{\"k\":\"v\"}}}"
        "]}}";
    /* alloc order per call: name dup, id dup; the 5th call hits the
     * realloc first (alloc 9) — fail it */
    ollama_test_set_alloc_fail(9);
    ck_assert_int_eq(count_calls(json_str), 4);
    ollama_test_set_alloc_fail(-1);
}
END_TEST

/* A str_dup failure inside an entry must leave the entry uncommitted
 * (count unchanged) with no leak (ASan-verified). */
START_TEST(test_parse_strdup_failure_leaves_entry_uncommitted)
{
    const char *json_str =
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"bash\",\"arguments\":{\"cmd\":\"ls\"}}}"
        "]}}";
    ollama_test_set_alloc_fail(1); /* first call: name dup */
    ck_assert_int_eq(count_calls(json_str), 0);
    ollama_test_set_alloc_fail(-1);
}
END_TEST

START_TEST(test_parse_succeeds_after_fault_injection_reset)
{
    const char *json_str =
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"bash\",\"arguments\":{\"cmd\":\"ls\"}}}"
        "]}}";
    ollama_test_set_alloc_fail(1);
    ck_assert_int_eq(count_calls(json_str), 0);
    ollama_test_set_alloc_fail(-1);
    ck_assert_int_eq(count_calls(json_str), 1);
}
END_TEST

Suite *ollama_tool_calls_suite(void)
{
    Suite *s = suite_create("ollama_tool_calls");

    TCase *tc_parse = tcase_create("NormalParsing");
    tcase_add_test(tc_parse, test_parse_extracts_name_and_arguments_from_single_tool_call);
    tcase_add_test(tc_parse, test_parse_extracts_all_tool_calls_from_array);
    tcase_add_test(tc_parse, test_parse_skips_entry_when_tool_call_name_is_empty);
    tcase_add_test(tc_parse, test_parse_returns_zero_when_tool_calls_field_is_missing);
    tcase_add_test(tc_parse, test_parse_invalid_json_returns_minus_one);
    tcase_add_test(tc_parse, test_parse_accumulates_tool_calls_across_repeated_invocations);
    tcase_add_test(tc_parse, test_parse_grows_capacity_when_tool_calls_exceed_initial_allocation);
    suite_add_tcase(s, tc_parse);

    TCase *tc_fault = tcase_create("FaultInjection");
    tcase_add_test(tc_fault, test_parse_realloc_failure_keeps_old_capacity_and_skips_overflow);
    tcase_add_test(tc_fault, test_parse_strdup_failure_leaves_entry_uncommitted);
    tcase_add_test(tc_fault, test_parse_succeeds_after_fault_injection_reset);
    suite_add_tcase(s, tc_fault);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s = ollama_tool_calls_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}
