/*
 * test_tool_memory.c - unit tests for the memory tool. Uses an in-memory
 * sqlite DB wired into a stub SessionManager registered through the
 * registry; no files, no network. Depends on: check, tool.h, registry.h,
 * session/memory, sqlite3.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>

#include "tools/tool.h"
#include "tools/registry.h"
#include "session/session_manager.h"
#include "session/memory.h"
#include "safety/safety.h"

Tool *tool_memory_create(SafetyConfig *safety);
void tool_memory_test_set_alloc_fail(int nth_allocation);

static void register_memory_db(void)
{
    static SessionManager sm;
    static int initialized = 0;
    if (initialized) return;
    memset(&sm, 0, sizeof(sm));
    ck_assert_int_eq(sqlite3_open(":memory:", &sm.db), SQLITE_OK);
    ck_assert_int_eq(memory_table_init(sm.db), 0);
    registry_set_session_manager(&sm);
    initialized = 1;
}

START_TEST(test_memory_set_get_delete_roundtrip)
{
    register_memory_db();
    Tool *tool = tool_memory_create(NULL);
    ck_assert_ptr_nonnull(tool);

    ToolResult *r = tool->execute(
        tool, "{\"action\":\"set\",\"key\":\"city\",\"value\":\"berlin\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    tool_result_free(r);

    r = tool->execute(tool, "{\"action\":\"get\",\"key\":\"city\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content, "berlin");
    tool_result_free(r);

    r = tool->execute(tool, "{\"action\":\"delete\",\"key\":\"city\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    tool_result_free(r);

    r = tool->execute(tool, "{\"action\":\"get\",\"key\":\"city\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content, "(not found)");
    tool_result_free(r);

    tool->destroy(tool);
}
END_TEST

START_TEST(test_memory_list_renders_all_facts)
{
    register_memory_db();
    Tool *tool = tool_memory_create(NULL);
    ck_assert_ptr_nonnull(tool);

    ToolResult *r = tool->execute(
        tool, "{\"action\":\"set\",\"key\":\"a\",\"value\":\"1\"}");
    tool_result_free(r);
    r = tool->execute(tool, "{\"action\":\"set\",\"key\":\"b\",\"value\":\"2\"}");
    tool_result_free(r);

    r = tool->execute(tool, "{\"action\":\"list\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_str_eq(r->content, "a: 1\nb: 2\n");
    tool_result_free(r);

    tool->destroy(tool);
}
END_TEST

START_TEST(test_memory_missing_action_is_validation_error)
{
    register_memory_db();
    Tool *tool = tool_memory_create(NULL);
    ck_assert_ptr_nonnull(tool);
    ToolResult *r = tool->execute(tool, "{}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    ck_assert_str_eq(r->error_category, "validation_error");
    tool_result_free(r);
    tool->destroy(tool);
}
END_TEST

/* tool_memory_create allocates: calloc(t), calloc(ctx), 3x str_dup; every
 * failure must return NULL without leaking (ASan-verified). */
START_TEST(test_tool_memory_create_allocation_failure_returns_null)
{
    for (int fail_at = 1; fail_at <= 3; fail_at++)
    {
        tool_memory_test_set_alloc_fail(fail_at);
        Tool *tool = tool_memory_create(NULL);
        ck_assert_ptr_null(tool);
    }
    tool_memory_test_set_alloc_fail(-1);
    Tool *tool = tool_memory_create(NULL);
    ck_assert_ptr_nonnull(tool);
    tool->destroy(tool);
}
END_TEST

/* The list path formats each fact with asprintf then grows the result
 * buffer with realloc; both are best-effort by design (a failed line is
 * skipped). Every failure must still yield a usable result without
 * error, never a crash or a lost count. */
START_TEST(test_memory_list_allocation_failure_degrades_gracefully)
{
    register_memory_db();
    Tool *tool = tool_memory_create(NULL);
    ck_assert_ptr_nonnull(tool);
    for (int fail_at = 1; fail_at <= 4; fail_at++)
    {
        tool_memory_test_set_alloc_fail(fail_at);
        ToolResult *r = tool->execute(tool, "{\"action\":\"list\"}");
        ck_assert_ptr_nonnull(r);
        ck_assert_ptr_null(r->error);
        tool_result_free(r);
    }
    tool_memory_test_set_alloc_fail(-1);
    tool->destroy(tool);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("ToolMemory");
    TCase *tc = tcase_create("Execute");
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_memory_set_get_delete_roundtrip);
    tcase_add_test(tc, test_memory_list_renders_all_facts);
    tcase_add_test(tc, test_memory_missing_action_is_validation_error);
    suite_add_tcase(suite, tc);

    TCase *tc_fault = tcase_create("FaultInjection");
    tcase_set_timeout(tc_fault, 30);
    tcase_add_test(tc_fault, test_tool_memory_create_allocation_failure_returns_null);
    tcase_add_test(tc_fault, test_memory_list_allocation_failure_degrades_gracefully);
    suite_add_tcase(suite, tc_fault);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
