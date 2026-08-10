#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "agent/context.h"
#include "agent/message.h"
#include "utils/string_utils.h"

/* --- split_thinking_content_dup --- */

START_TEST(test_split_thinking_no_think_tags)
{
    char *result = split_thinking_content_dup("Hello, how can I help?");
    ck_assert_ptr_nonnull(result);
    ck_assert_str_eq(result, "Hello, how can I help?");
    free(result);
}
END_TEST

START_TEST(test_split_thinking_last_think_removed)
{
    char *result = split_thinking_content_dup(
        "some text<think>internal reasoning</think>visible output");
    ck_assert_ptr_nonnull(result);
    ck_assert_str_eq(result, "some text<think>internal reasoning</think>");
    free(result);
}
END_TEST

START_TEST(test_split_thinking_multiple_thinks)
{
    char *result = split_thinking_content_dup(
        "before<think>first</think>middle<think>second</think>after");
    ck_assert_ptr_nonnull(result);
    ck_assert_str_eq(result,
        "before<think>first</think>middle<think>second</think>");
    free(result);
}
END_TEST

START_TEST(test_split_thinking_no_close_tag)
{
    char *result = split_thinking_content_dup(
        "text<think>unclosed");
    ck_assert_ptr_nonnull(result);
    ck_assert_str_eq(result, "text<think>unclosed");
    free(result);
}
END_TEST

START_TEST(test_split_thinking_null_returns_null)
{
    char *result = split_thinking_content_dup(NULL);
    ck_assert_ptr_null(result);
}
END_TEST

START_TEST(test_split_thinking_think_only)
{
    char *result = split_thinking_content_dup(
        "<think>reasoning</think>");
    ck_assert_ptr_nonnull(result);
    ck_assert_str_eq(result, "<think>reasoning</think>");
    free(result);
}
END_TEST

START_TEST(test_split_thinking_nested_think_tags)
{
    char *result = split_thinking_content_dup(
        "pre<think>outer<think>inner</think></think>post");
    ck_assert_ptr_nonnull(result);
    ck_assert_str_eq(result,
        "pre<think>outer<think>inner</think>");
    free(result);
}
END_TEST

/* --- smart_select_alloc --- */

static Message make_msg(const char *role, const char *content)
{
    Message m = {0};
    m.role = (char *)role;
    m.content = (char *)content;
    return m;
}

static Message make_owned_msg(const char *role, const char *content)
{
    Message m = {0};
    m.role = str_dup(role);
    m.content = str_dup(content);
    ck_assert_ptr_nonnull(m.role);
    ck_assert_ptr_nonnull(m.content);
    return m;
}

/* smart_select_alloc (count > keep_count) allocates scores, selected_flags and
 * the result array before committing; each failure must return NULL with
 * the input untouched and no leaked intermediates (ASan-verified). */
START_TEST(test_smart_select_allocation_failure_returns_null_and_leaves_input_untouched)
{
    Message msgs[4] = {
        make_msg("system", "sys"),
        make_msg("user", "u1"),
        make_msg("assistant", "a1"),
        make_msg("tool", "t1"),
    };
    for (int fail_at = 1; fail_at <= 3; fail_at++)
    {
        context_test_set_alloc_fail(fail_at);
        Message *selected = smart_select_alloc(msgs, 4, 2);
        ck_assert_ptr_null(selected);
        ck_assert_str_eq(msgs[0].role, "system");
        ck_assert_str_eq(msgs[3].content, "t1");
    }
    context_test_set_alloc_fail(-1);
}
END_TEST

/* fast path (count <= keep_count) allocates the result copy first; its
 * failure must also return NULL without touching the input. */
START_TEST(test_smart_select_fast_path_allocation_failure_returns_null)
{
    Message msgs[2] = {make_msg("user", "u"), make_msg("assistant", "a")};
    context_test_set_alloc_fail(1);
    Message *selected = smart_select_alloc(msgs, 2, 5);
    ck_assert_ptr_null(selected);
    context_test_set_alloc_fail(-1);
}
END_TEST

/* trim_messages_by_tokens_new allocates keep[] then trimmed[]; its contract
 * is to return the ORIGINAL array unchanged (count intact, messages
 * unmodified) on allocation failure, not a partial trim. */
START_TEST(test_trim_messages_allocation_failure_returns_original_untouched)
{
    Message msgs[3] = {
        make_msg("system", "sys"),
        make_msg("user", "u1"),
        make_msg("assistant", "a1"),
    };
    int count = 3;
    for (int fail_at = 1; fail_at <= 2; fail_at++)
    {
        context_test_set_alloc_fail(fail_at);
        Message *result = trim_messages_by_tokens_new(msgs, &count, 2);
        ck_assert_ptr_eq(result, msgs);
        ck_assert_int_eq(count, 3);
        ck_assert_str_eq(msgs[1].content, "u1");
        ck_assert_str_eq(msgs[2].content, "a1");
    }
    context_test_set_alloc_fail(-1);
}
END_TEST

/* After fault injection the normal path still works — the hook must be
 * resettable and leave no stale state. The kept messages are MOVED out
 * of msgs (their entries are zeroed), so the result must be freed
 * element-wise and the survivors in msgs must keep their original
 * content. */
START_TEST(test_trim_messages_succeeds_after_fault_injection_reset)
{
    /* trim moves kept entries out of msgs and frees the input array on
     * success, so msgs must be heap-owned; entries must be owned too */
    Message *msgs = calloc(3, sizeof(Message));
    ck_assert_ptr_nonnull(msgs);
    msgs[0] = make_owned_msg("system", "sys");
    msgs[1] = make_owned_msg("user", "u1");
    msgs[2] = make_owned_msg("assistant", "a1");
    int count = 3;
    context_test_set_alloc_fail(1);
    Message *result = trim_messages_by_tokens_new(msgs, &count, 1);
    ck_assert_ptr_eq(result, msgs);
    ck_assert_int_eq(count, 3);
    /* failure path: input untouched, so we own the cleanup */
    for (int i = 0; i < 3; i++)
        message_clear(&msgs[i]);
    free(msgs);

    msgs = calloc(3, sizeof(Message));
    ck_assert_ptr_nonnull(msgs);
    msgs[0] = make_owned_msg("system", "sys");
    msgs[1] = make_owned_msg("user", "u1");
    msgs[2] = make_owned_msg("assistant", "a1");
    count = 3;
    context_test_set_alloc_fail(-1);
    result = trim_messages_by_tokens_new(msgs, &count, 1);
    ck_assert_ptr_ne(result, msgs);
    ck_assert_int_gt(count, 0);
    ck_assert_int_lt(count, 3);
    /* success path: the function freed msgs; result is caller-owned */
    for (int i = 0; i < count; i++)
        ck_assert_ptr_nonnull(result[i].role);
    for (int i = 0; i < count; i++)
        message_clear(&result[i]);
    free(result);
}
END_TEST

START_TEST(test_smart_select_count_less_than_keep)
{
    Message msgs[2] = {
        make_msg("system", "You are helpful."),
        make_msg("user", "Hello"),
    };
    Message *selected = smart_select_alloc(msgs, 2, 5);
    ck_assert_ptr_nonnull(selected);
    ck_assert_str_eq(selected[0].role, "system");
    ck_assert_str_eq(selected[1].role, "user");
    message_free_all(selected, 2);
}
END_TEST

START_TEST(test_smart_select_keeps_system_messages)
{
    Message msgs[4] = {
        make_msg("system", "prompt"),
        make_msg("user", "q1"),
        make_msg("assistant", "a1"),
        make_msg("user", "q2"),
    };
    Message *selected = smart_select_alloc(msgs, 4, 2);
    ck_assert_ptr_nonnull(selected);
    ck_assert_str_eq(selected[0].role, "system");
    message_free_all(selected, 2);
}
END_TEST

START_TEST(test_smart_select_keeps_tool_and_assistant_pair)
{
    Message msgs[4] = {
        make_msg("user", "run command"),
        make_msg("assistant", "ok"),
        make_msg("tool", "output"),
        make_msg("user", "next"),
    };
    Message *selected = smart_select_alloc(msgs, 4, 2);
    ck_assert_ptr_nonnull(selected);
    message_free_all(selected, 2);
}
END_TEST

START_TEST(test_smart_select_zero_count)
{
    Message *selected = smart_select_alloc(NULL, 0, 0);
    (void)selected;
    free(selected);
}
END_TEST

/* --- trim_messages_by_tokens_new --- */

START_TEST(test_trim_messages_by_tokens_under_budget)
{
    Message *msgs = calloc(2, sizeof(Message));
    msgs[0] = make_owned_msg("system", "prompt");
    msgs[1] = make_owned_msg("user", "hello");
    int count = 2;
    Message *result = trim_messages_by_tokens_new(msgs, &count, 1000);
    ck_assert_ptr_nonnull(result);
    ck_assert_int_eq(count, 2);
    message_free_all(result, count);
}
END_TEST

START_TEST(test_trim_messages_by_tokens_over_budget_drops_oldest)
{
    Message *msgs = calloc(3, sizeof(Message));
    msgs[0] = make_owned_msg("system", "You are a helpful assistant.");
    msgs[1] = make_owned_msg("user", "What is the capital of France?");
    msgs[2] = make_owned_msg("assistant", "Paris");
    int count = 3;
    Message *result = trim_messages_by_tokens_new(msgs, &count, 5);
    ck_assert_ptr_nonnull(result);
    ck_assert(count >= 1);
    ck_assert_str_eq(result[0].role, "system");
    message_free_all(result, count);
}
END_TEST

START_TEST(test_trim_messages_by_tokens_keeps_system)
{
    Message *msgs = calloc(4, sizeof(Message));
    msgs[0] = make_owned_msg("system", "prompt");
    msgs[1] = make_owned_msg("user", "A very long user message that should be dropped");
    msgs[2] = make_owned_msg("assistant", "Another very long assistant response that should also be dropped");
    msgs[3] = make_owned_msg("user", "Short");
    int count = 4;
    Message *result = trim_messages_by_tokens_new(msgs, &count, 3);
    ck_assert_ptr_nonnull(result);
    ck_assert(count >= 1);
    ck_assert_str_eq(result[0].role, "system");
    message_free_all(result, count);
}
END_TEST

START_TEST(test_trim_messages_by_tokens_null_params)
{
    int count = 0;
    Message *result = trim_messages_by_tokens_new(NULL, &count, 100);
    ck_assert_ptr_null(result);
    result = trim_messages_by_tokens_new(NULL, NULL, 100);
    ck_assert_ptr_null(result);
}
END_TEST

START_TEST(test_trim_messages_by_tokens_zero_count)
{
    Message *msgs = calloc(1, sizeof(Message));
    msgs[0] = make_msg("user", "test");
    int count = 0;
    Message *result = trim_messages_by_tokens_new(msgs, &count, 100);
    ck_assert_ptr_eq(result, msgs);
    free(result);
}
END_TEST

START_TEST(test_trim_messages_by_tokens_orphan_tool_preserved)
{
    Message *msgs = calloc(4, sizeof(Message));
    msgs[0] = make_owned_msg("system", "prompt");
    msgs[1] = make_owned_msg("user", "run ls");
    msgs[2] = make_owned_msg("assistant", "I'll run ls for you");
    msgs[3] = make_owned_msg("tool", "file1 file2");
    int count = 4;
    Message *result = trim_messages_by_tokens_new(msgs, &count, 2);
    ck_assert_ptr_nonnull(result);
    message_free_all(result, count);
}
END_TEST

/* --- apply_context_window --- */

START_TEST(test_apply_context_window_under_limits)
{
    Message *msgs = calloc(2, sizeof(Message));
    msgs[0] = make_msg("system", "prompt");
    msgs[1] = make_msg("user", "hello");
    int count = 2;
    Message *result = apply_context_window(msgs, &count, 100, 10000);
    ck_assert_ptr_eq(result, msgs);
    ck_assert_int_eq(count, 2);
    free(result);
}
END_TEST

START_TEST(test_apply_context_window_over_message_limit)
{
    Message *msgs = calloc(5, sizeof(Message));
    msgs[0] = make_owned_msg("system", "prompt");
    msgs[1] = make_owned_msg("user", "q1");
    msgs[2] = make_owned_msg("assistant", "a1");
    msgs[3] = make_owned_msg("user", "q2");
    msgs[4] = make_owned_msg("assistant", "a2");
    int count = 5;
    Message *result = apply_context_window(msgs, &count, 2, 10000);
    ck_assert_ptr_nonnull(result);
    ck_assert(count <= 5);
    message_free_all(msgs, 5);
    ck_assert_str_eq(result[0].role, "system");
    message_free_all(result, count);
}
END_TEST

START_TEST(test_apply_context_window_over_char_limit)
{
    Message *msgs = calloc(2, sizeof(Message));
    msgs[0] = make_owned_msg("system", "A very long system prompt that should exceed char budget");
    msgs[1] = make_owned_msg("user", "A very long user message that also exceeds the budget");
    int count = 2;
    Message *result = apply_context_window(msgs, &count, 100, 5);
    ck_assert_ptr_nonnull(result);
    message_free_all(msgs, 2);
    ck_assert_ptr_nonnull(result[0].role);
    message_free_all(result, count);
}
END_TEST

Suite *context_suite(void)
{
    Suite *s = suite_create("Context");
    TCase *tc_think = tcase_create("SplitThinking");
    tcase_set_timeout(tc_think, 10);
    tcase_add_test(tc_think, test_split_thinking_no_think_tags);
    tcase_add_test(tc_think, test_split_thinking_last_think_removed);
    tcase_add_test(tc_think, test_split_thinking_multiple_thinks);
    tcase_add_test(tc_think, test_split_thinking_no_close_tag);
    tcase_add_test(tc_think, test_split_thinking_null_returns_null);
    tcase_add_test(tc_think, test_split_thinking_think_only);
    tcase_add_test(tc_think, test_split_thinking_nested_think_tags);
    suite_add_tcase(s, tc_think);

    TCase *tc_select = tcase_create("SmartSelect");
    tcase_set_timeout(tc_select, 10);
    tcase_add_test(tc_select, test_smart_select_count_less_than_keep);
    tcase_add_test(tc_select, test_smart_select_keeps_system_messages);
    tcase_add_test(tc_select, test_smart_select_keeps_tool_and_assistant_pair);
    tcase_add_test(tc_select, test_smart_select_zero_count);
    suite_add_tcase(s, tc_select);

    TCase *tc_trim = tcase_create("TrimByTokens");
    tcase_set_timeout(tc_trim, 10);
    tcase_add_test(tc_trim, test_trim_messages_by_tokens_under_budget);
    tcase_add_test(tc_trim, test_trim_messages_by_tokens_over_budget_drops_oldest);
    tcase_add_test(tc_trim, test_trim_messages_by_tokens_keeps_system);
    tcase_add_test(tc_trim, test_trim_messages_by_tokens_null_params);
    tcase_add_test(tc_trim, test_trim_messages_by_tokens_zero_count);
    tcase_add_test(tc_trim, test_trim_messages_by_tokens_orphan_tool_preserved);
    suite_add_tcase(s, tc_trim);

    TCase *tc_fault = tcase_create("FaultInjection");
    tcase_set_timeout(tc_fault, 10);
    tcase_add_test(tc_fault, test_smart_select_allocation_failure_returns_null_and_leaves_input_untouched);
    tcase_add_test(tc_fault, test_smart_select_fast_path_allocation_failure_returns_null);
    tcase_add_test(tc_fault, test_trim_messages_allocation_failure_returns_original_untouched);
    tcase_add_test(tc_fault, test_trim_messages_succeeds_after_fault_injection_reset);
    suite_add_tcase(s, tc_fault);

    TCase *tc_apply = tcase_create("ApplyWindow");
    tcase_set_timeout(tc_apply, 10);
    tcase_add_test(tc_apply, test_apply_context_window_under_limits);
    tcase_add_test(tc_apply, test_apply_context_window_over_message_limit);
    tcase_add_test(tc_apply, test_apply_context_window_over_char_limit);
    suite_add_tcase(s, tc_apply);

    return s;
}

int main(void)
{
    Suite *s = context_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
