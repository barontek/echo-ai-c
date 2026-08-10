/*
 * test_tool_ask_user.c - unit tests for the ask_user tool using the
 * registry's callback path (no stdin). Depends on: check, tool.h,
 * registry, safety.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "tools/tool.h"
#include "tools/registry.h"
#include "utils/string_utils.h"

Tool *tool_ask_user_create(SafetyConfig *safety);

static char *captured_question = NULL;

static char *stub_answer(const char *question, void *userdata)
{
    (void)userdata;
    free(captured_question);
    captured_question = str_dup(question);
    return str_dup("the answer");
}

static char *stub_cancel(const char *question, void *userdata)
{
    (void)question;
    (void)userdata;
    return NULL;
}

START_TEST(test_ask_user_uses_callback_and_returns_answer)
{
    registry_set_ask_user_callback(stub_answer, NULL);
    Tool *tool = tool_ask_user_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"question\":\"proceed?\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content, "the answer");
    ck_assert_str_eq(captured_question, "proceed?");
    tool_result_free(r);
    tool->destroy(tool);
    registry_set_ask_user_callback(NULL, NULL);
    free(captured_question);
    captured_question = NULL;
}
END_TEST

START_TEST(test_ask_user_null_callback_answer_is_cancelled)
{
    registry_set_ask_user_callback(stub_cancel, NULL);
    Tool *tool = tool_ask_user_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{\"question\":\"abort?\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "cancelled");
    tool_result_free(r);
    tool->destroy(tool);
    registry_set_ask_user_callback(NULL, NULL);
}
END_TEST

START_TEST(test_ask_user_missing_question_is_validation_error)
{
    Tool *tool = tool_ask_user_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("AskUser");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_ask_user_uses_callback_and_returns_answer);
    tcase_add_test(tc, test_ask_user_null_callback_answer_is_cancelled);
    tcase_add_test(tc, test_ask_user_missing_question_is_validation_error);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
