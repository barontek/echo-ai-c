#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "tools/tool.h"
#include "tools/registry.h"
#include "llm/provider.h"
#include "safety/safety.h"
#include "utils/string_utils.h"

extern Tool *tool_delegate_create(SafetyConfig *safety);
extern void tool_delegate_test_set_alloc_fail(int nth_allocation);

/* Mock registry functions — avoid linking the whole registry.c tool chain */
int registry_get_delegate_config(const char **provider_name, const char **base_url,
                                  const char **model, int *num_ctx, int *keep_alive_secs,
                                  double *temperature, int *timeout, int *max_iterations)
{
    if (provider_name) *provider_name = "mock";
    if (base_url) *base_url = "http://localhost";
    if (model) *model = "mock-model";
    if (num_ctx) *num_ctx = 4096;
    if (keep_alive_secs) *keep_alive_secs = 120;
    if (temperature) *temperature = 0.7;
    if (timeout) *timeout = 30;
    if (max_iterations) *max_iterations = 10;
    return 0;
}

Tool *registry_get(const char *name) { (void)name; return NULL; }

static LLMResponse *mock_chat(LLMProvider *self, Message *messages, int count,
                               const char *model, double temperature, int timeout,
                               const char *tools_json)
{
    (void)self; (void)messages; (void)count; (void)model; (void)temperature; (void)timeout;
    (void)tools_json;
    LLMResponse *resp = calloc(1, sizeof(LLMResponse));
    if (resp)
    {
        resp->content = str_dup("mock response with no tool calls");
        resp->tool_calls_count = 0;
    }
    return resp;
}

static void mock_destroy(LLMProvider *self) { free(self); }

LLMProvider *td_test_get_provider(const char *name, const char *model,
                                   const char *base_url, int num_ctx, int keep_alive_secs)
{
    (void)name; (void)model; (void)base_url; (void)num_ctx; (void)keep_alive_secs;
    LLMProvider *p = calloc(1, sizeof(LLMProvider));
    if (p)
    {
        p->chat = mock_chat;
        p->destroy = mock_destroy;
    }
    return p;
}

START_TEST(test_delegate_normal)
{
    Tool *t = tool_delegate_create(NULL);
    ck_assert_ptr_nonnull(t);

    ToolResult *r = t->execute(t, "{\"task\":\"test the delegate tool\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_nonnull(r->content);
    tool_result_free(r);
    t->destroy(t);
}
END_TEST

/* UAF regression: old code read task_str after cJSON_Delete(args) freed it.
   Under ASan this is a heap-use-after-free crash.  Fix: str_dup before delete. */
START_TEST(test_delegate_uaf_task_str)
{
    Tool *t = tool_delegate_create(NULL);
    ck_assert_ptr_nonnull(t);

    ToolResult *r = t->execute(t, "{\"task\":\"UAF should not crash under ASan\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_null(r->error);
    ck_assert_ptr_nonnull(r->content);
    tool_result_free(r);
    t->destroy(t);
}
END_TEST

/* tool_delegate_create uses str_dup 3 times (name, description, schema),
   so delegate_execute's str_dups start at position 4+ */
START_TEST(test_delegate_alloc_fail_task_str)
{
    tool_delegate_test_set_alloc_fail(4);
    Tool *t = tool_delegate_create(NULL);
    ck_assert_ptr_nonnull(t);

    ToolResult *r = t->execute(t, "{\"task\":\"test\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    tool_result_free(r);
    t->destroy(t);
}
END_TEST

START_TEST(test_delegate_alloc_fail_sys_role)
{
    tool_delegate_test_set_alloc_fail(5);
    Tool *t = tool_delegate_create(NULL);
    ck_assert_ptr_nonnull(t);

    ToolResult *r = t->execute(t, "{\"task\":\"test\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    tool_result_free(r);
    t->destroy(t);
}
END_TEST

START_TEST(test_delegate_alloc_fail_sys_content)
{
    tool_delegate_test_set_alloc_fail(6);
    Tool *t = tool_delegate_create(NULL);
    ck_assert_ptr_nonnull(t);

    ToolResult *r = t->execute(t, "{\"task\":\"test\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    tool_result_free(r);
    t->destroy(t);
}
END_TEST

START_TEST(test_delegate_alloc_fail_user_role)
{
    tool_delegate_test_set_alloc_fail(7);
    Tool *t = tool_delegate_create(NULL);
    ck_assert_ptr_nonnull(t);

    ToolResult *r = t->execute(t, "{\"task\":\"test\"}");
    ck_assert_ptr_nonnull(r);
    ck_assert_ptr_nonnull(r->error);
    tool_result_free(r);
    t->destroy(t);
}
END_TEST

Suite *tool_delegate_suite(void)
{
    Suite *s = suite_create("ToolDelegate");
    TCase *tc = tcase_create("Core");
    tcase_add_test(tc, test_delegate_normal);
    tcase_add_test(tc, test_delegate_uaf_task_str);
    tcase_add_test(tc, test_delegate_alloc_fail_task_str);
    tcase_add_test(tc, test_delegate_alloc_fail_sys_role);
    tcase_add_test(tc, test_delegate_alloc_fail_sys_content);
    tcase_add_test(tc, test_delegate_alloc_fail_user_role);
    suite_add_tcase(s, tc);
    return s;
}

int main(void)
{
    Suite *s = tool_delegate_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
