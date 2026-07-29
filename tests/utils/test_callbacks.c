#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "utils/callbacks.h"

typedef struct {
    int run_start_calls;
    int run_end_calls;
    int run_error_calls;
    int llm_start_calls;
    int llm_end_calls;
    int tool_start_calls;
    int tool_end_calls;
    int tool_error_calls;
    const char *last_run_id;
    const char *last_tool_name;
    int last_message_count;
} SpyData;

static void spy_run_start(const char *run_id, const char *prompt, void *userdata)
{
    SpyData *spy = (SpyData *)userdata;
    spy->run_start_calls++;
    spy->last_run_id = run_id;
    (void)prompt;
}

static void spy_run_end(const char *run_id, const char *response, void *userdata)
{
    SpyData *spy = (SpyData *)userdata;
    spy->run_end_calls++;
    spy->last_run_id = run_id;
    (void)response;
}

static void spy_run_error(const char *run_id, const char *error, void *userdata)
{
    SpyData *spy = (SpyData *)userdata;
    spy->run_error_calls++;
    spy->last_run_id = run_id;
    (void)error;
}

static void spy_llm_start(const char *run_id, int message_count, void *userdata)
{
    SpyData *spy = (SpyData *)userdata;
    spy->llm_start_calls++;
    spy->last_run_id = run_id;
    spy->last_message_count = message_count;
}

static void spy_llm_end(const char *run_id, const char *response, void *userdata)
{
    SpyData *spy = (SpyData *)userdata;
    spy->llm_end_calls++;
    spy->last_run_id = run_id;
    (void)response;
}

static void spy_tool_start(const char *run_id, const char *tool_name,
                           const char *args, void *userdata)
{
    SpyData *spy = (SpyData *)userdata;
    spy->tool_start_calls++;
    spy->last_run_id = run_id;
    spy->last_tool_name = tool_name;
    (void)args;
}

static void spy_tool_end(const char *run_id, const char *tool_name,
                         const char *result, void *userdata)
{
    SpyData *spy = (SpyData *)userdata;
    spy->tool_end_calls++;
    spy->last_run_id = run_id;
    spy->last_tool_name = tool_name;
    (void)result;
}

static void spy_tool_error(const char *run_id, const char *tool_name,
                           const char *error, void *userdata)
{
    SpyData *spy = (SpyData *)userdata;
    spy->tool_error_calls++;
    spy->last_run_id = run_id;
    spy->last_tool_name = tool_name;
    (void)error;
}

static CallbackHooks make_spy_hooks(SpyData *spy)
{
    CallbackHooks h = {0};
    h.on_run_start = spy_run_start;
    h.on_run_end = spy_run_end;
    h.on_run_error = spy_run_error;
    h.on_llm_start = spy_llm_start;
    h.on_llm_end = spy_llm_end;
    h.on_tool_start = spy_tool_start;
    h.on_tool_end = spy_tool_end;
    h.on_tool_error = spy_tool_error;
    h.userdata = spy;
    return h;
}

START_TEST(test_cb_create_destroy)
{
    CallbackManager *mgr = cb_manager_create();
    ck_assert_ptr_nonnull(mgr);
    ck_assert_int_eq(mgr->count, 0);
    cb_manager_destroy(mgr);
}
END_TEST

START_TEST(test_cb_register_and_count)
{
    CallbackManager *mgr = cb_manager_create();
    SpyData spy = {0};
    CallbackHooks h = make_spy_hooks(&spy);
    ck_assert_int_eq(cb_manager_register(mgr, h), 0);
    ck_assert_int_eq(mgr->count, 1);
    cb_manager_destroy(mgr);
}
END_TEST

START_TEST(test_cb_register_multiple)
{
    CallbackManager *mgr = cb_manager_create();
    SpyData spy1 = {0}, spy2 = {0};
    ck_assert_int_eq(cb_manager_register(mgr, make_spy_hooks(&spy1)), 0);
    ck_assert_int_eq(cb_manager_register(mgr, make_spy_hooks(&spy2)), 0);
    ck_assert_int_eq(mgr->count, 2);
    cb_manager_destroy(mgr);
}
END_TEST

START_TEST(test_cb_register_up_to_max)
{
    CallbackManager *mgr = cb_manager_create();
    SpyData spies[MAX_CALLBACKS];
    memset(spies, 0, sizeof(spies));
    for (int i = 0; i < MAX_CALLBACKS; i++)
    {
        ck_assert_int_eq(cb_manager_register(mgr, make_spy_hooks(&spies[i])), 0);
    }
    ck_assert_int_eq(mgr->count, MAX_CALLBACKS);
    SpyData extra = {0};
    ck_assert_int_eq(cb_manager_register(mgr, make_spy_hooks(&extra)), -1);
    ck_assert_int_eq(mgr->count, MAX_CALLBACKS);
    cb_manager_destroy(mgr);
}
END_TEST

START_TEST(test_cb_dispatch_run_start)
{
    CallbackManager *mgr = cb_manager_create();
    SpyData spy = {0};
    cb_manager_register(mgr, make_spy_hooks(&spy));
    cb_manager_run_start(mgr, "run1", "hello");
    ck_assert_int_eq(spy.run_start_calls, 1);
    ck_assert_str_eq(spy.last_run_id, "run1");
    cb_manager_destroy(mgr);
}
END_TEST

START_TEST(test_cb_dispatch_run_end)
{
    CallbackManager *mgr = cb_manager_create();
    SpyData spy = {0};
    cb_manager_register(mgr, make_spy_hooks(&spy));
    cb_manager_run_end(mgr, "run1", "goodbye");
    ck_assert_int_eq(spy.run_end_calls, 1);
    ck_assert_str_eq(spy.last_run_id, "run1");
    cb_manager_destroy(mgr);
}
END_TEST

START_TEST(test_cb_dispatch_run_error)
{
    CallbackManager *mgr = cb_manager_create();
    SpyData spy = {0};
    cb_manager_register(mgr, make_spy_hooks(&spy));
    cb_manager_run_error(mgr, "run1", "oops");
    ck_assert_int_eq(spy.run_error_calls, 1);
    ck_assert_str_eq(spy.last_run_id, "run1");
    cb_manager_destroy(mgr);
}
END_TEST

START_TEST(test_cb_dispatch_llm_start)
{
    CallbackManager *mgr = cb_manager_create();
    SpyData spy = {0};
    cb_manager_register(mgr, make_spy_hooks(&spy));
    cb_manager_llm_start(mgr, "run1", 5);
    ck_assert_int_eq(spy.llm_start_calls, 1);
    ck_assert_int_eq(spy.last_message_count, 5);
    cb_manager_destroy(mgr);
}
END_TEST

START_TEST(test_cb_dispatch_llm_end)
{
    CallbackManager *mgr = cb_manager_create();
    SpyData spy = {0};
    cb_manager_register(mgr, make_spy_hooks(&spy));
    cb_manager_llm_end(mgr, "run1", "response text");
    ck_assert_int_eq(spy.llm_end_calls, 1);
    cb_manager_destroy(mgr);
}
END_TEST

START_TEST(test_cb_dispatch_tool_start)
{
    CallbackManager *mgr = cb_manager_create();
    SpyData spy = {0};
    cb_manager_register(mgr, make_spy_hooks(&spy));
    cb_manager_tool_start(mgr, "run1", "bash", "{\"cmd\":\"ls\"}");
    ck_assert_int_eq(spy.tool_start_calls, 1);
    ck_assert_str_eq(spy.last_tool_name, "bash");
    cb_manager_destroy(mgr);
}
END_TEST

START_TEST(test_cb_dispatch_tool_end)
{
    CallbackManager *mgr = cb_manager_create();
    SpyData spy = {0};
    cb_manager_register(mgr, make_spy_hooks(&spy));
    cb_manager_tool_end(mgr, "run1", "bash", "file1\nfile2");
    ck_assert_int_eq(spy.tool_end_calls, 1);
    ck_assert_str_eq(spy.last_tool_name, "bash");
    cb_manager_destroy(mgr);
}
END_TEST

START_TEST(test_cb_dispatch_tool_error)
{
    CallbackManager *mgr = cb_manager_create();
    SpyData spy = {0};
    cb_manager_register(mgr, make_spy_hooks(&spy));
    cb_manager_tool_error(mgr, "run1", "bash", "permission denied");
    ck_assert_int_eq(spy.tool_error_calls, 1);
    ck_assert_str_eq(spy.last_tool_name, "bash");
    cb_manager_destroy(mgr);
}
END_TEST

START_TEST(test_cb_dispatch_multiple_hooks)
{
    CallbackManager *mgr = cb_manager_create();
    SpyData spy1 = {0}, spy2 = {0};
    cb_manager_register(mgr, make_spy_hooks(&spy1));
    cb_manager_register(mgr, make_spy_hooks(&spy2));
    cb_manager_run_start(mgr, "runX", "prompt");
    ck_assert_int_eq(spy1.run_start_calls, 1);
    ck_assert_int_eq(spy2.run_start_calls, 1);
    ck_assert_str_eq(spy1.last_run_id, "runX");
    ck_assert_str_eq(spy2.last_run_id, "runX");
    cb_manager_destroy(mgr);
}
END_TEST

START_TEST(test_cb_dispatch_sparse_hooks)
{
    CallbackManager *mgr = cb_manager_create();
    SpyData spy = {0};
    CallbackHooks h = {0};
    h.on_run_start = spy_run_start;
    h.on_llm_end = spy_llm_end;
    h.userdata = &spy;
    cb_manager_register(mgr, h);

    cb_manager_run_start(mgr, "r1", "p");
    ck_assert_int_eq(spy.run_start_calls, 1);
    ck_assert_int_eq(spy.llm_end_calls, 0);

    cb_manager_llm_end(mgr, "r2", "resp");
    ck_assert_int_eq(spy.llm_end_calls, 1);
    ck_assert_int_eq(spy.run_start_calls, 1);

    cb_manager_tool_start(mgr, "r3", "t", "{}");
    ck_assert_int_eq(spy.tool_start_calls, 0);

    cb_manager_destroy(mgr);
}
END_TEST

START_TEST(test_cb_null_mgr_safe)
{
    cb_manager_destroy(NULL);
    ck_assert_int_eq(cb_manager_register(NULL, (CallbackHooks){0}), -1);
    cb_manager_run_start(NULL, "r", "p");
    cb_manager_run_end(NULL, "r", "p");
    cb_manager_run_error(NULL, "r", "e");
    cb_manager_llm_start(NULL, "r", 0);
    cb_manager_llm_end(NULL, "r", "p");
    cb_manager_tool_start(NULL, "r", "t", "a");
    cb_manager_tool_end(NULL, "r", "t", "x");
    cb_manager_tool_error(NULL, "r", "t", "e");
}
END_TEST

Suite *callbacks_suite(void)
{
    Suite *s = suite_create("Callbacks");
    TCase *tc = tcase_create("Lifecycle");
    tcase_set_timeout(tc, 10);
    tcase_add_test(tc, test_cb_create_destroy);
    tcase_add_test(tc, test_cb_register_and_count);
    tcase_add_test(tc, test_cb_register_multiple);
    tcase_add_test(tc, test_cb_register_up_to_max);
    suite_add_tcase(s, tc);

    TCase *tc2 = tcase_create("Dispatch");
    tcase_set_timeout(tc2, 10);
    tcase_add_test(tc2, test_cb_dispatch_run_start);
    tcase_add_test(tc2, test_cb_dispatch_run_end);
    tcase_add_test(tc2, test_cb_dispatch_run_error);
    tcase_add_test(tc2, test_cb_dispatch_llm_start);
    tcase_add_test(tc2, test_cb_dispatch_llm_end);
    tcase_add_test(tc2, test_cb_dispatch_tool_start);
    tcase_add_test(tc2, test_cb_dispatch_tool_end);
    tcase_add_test(tc2, test_cb_dispatch_tool_error);
    tcase_add_test(tc2, test_cb_dispatch_multiple_hooks);
    tcase_add_test(tc2, test_cb_dispatch_sparse_hooks);
    suite_add_tcase(s, tc2);

    TCase *tc3 = tcase_create("NullSafety");
    tcase_set_timeout(tc3, 10);
    tcase_add_test(tc3, test_cb_null_mgr_safe);
    suite_add_tcase(s, tc3);

    return s;
}

int main(void)
{
    Suite *s = callbacks_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
