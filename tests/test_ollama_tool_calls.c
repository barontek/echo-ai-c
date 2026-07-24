/* Fault-injection tests for the tool_calls parsing logic in ollama.c.
 *
 * Uses the AGENTS.md section 10 pattern: redefines str_dup to simulate
 * allocation failures at specific call sites without needing a real OOM.
 *
 * The parse_stream_tool_calls function below mirrors the static function
 * of the same name in src/llm/ollama.c.  Keep them in sync.
 */
#define OLLAMA_TOOL_CALLS_TEST 1
#include <stdlib.h>
#include <string.h>
#include <check.h>
#include <cjson/cJSON.h>

/* ---- WriteBuf mirror (must match src/llm/ollama.c) ---- */
typedef struct {
    char *id;
    char *name;
    char *arguments;
} ToolCall;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int thinking_open;
    void (*on_chunk)(const char *, void *);
    void *userdata;
    ToolCall *tool_calls;
    int tool_calls_count;
    int tool_calls_cap;
} WriteBuf;

/* ---- str_dup with fault-injection ---- */
static int strdup_fail_at = -1;
static int strdup_call_count = 0;

static char *test_strdup(const char *s)
{
    strdup_call_count++;
    if (strdup_call_count == strdup_fail_at) return NULL;
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

#define str_dup test_strdup

/* ---- tool_call_free (mirrors message.c) ---- */
static void tool_call_free_mirror(ToolCall *call)
{
    if (!call) return;
    free(call->id);
    free(call->name);
    free(call->arguments);
}

/* ---- parse_stream_tool_calls (mirrors ollama.c, must stay in sync) ---- */
static void parse_stream_tool_calls(WriteBuf *buf, cJSON *msg)
{
    cJSON *tc_arr = cJSON_GetObjectItem(msg, "tool_calls");
    if (!tc_arr || !cJSON_IsArray(tc_arr)) return;

    int tc_count = cJSON_GetArraySize(tc_arr);
    for (int t = 0; t < tc_count; t++)
    {
        cJSON *tc = cJSON_GetArrayItem(tc_arr, t);
        cJSON *fn = cJSON_GetObjectItem(tc, "function");
        if (!fn) continue;
        cJSON *tname = cJSON_GetObjectItem(fn, "name");
        cJSON *args = cJSON_GetObjectItem(fn, "arguments");

        if (buf->tool_calls_count >= buf->tool_calls_cap)
        {
            int new_cap = buf->tool_calls_cap == 0 ? 4 : buf->tool_calls_cap * 2;
            ToolCall *new_tc = realloc(buf->tool_calls,
                                        sizeof(ToolCall) * (size_t)new_cap);
            if (new_tc)
            {
                memset(new_tc + buf->tool_calls_cap, 0,
                       sizeof(ToolCall) * (size_t)(new_cap - buf->tool_calls_cap));
                buf->tool_calls = new_tc;
                buf->tool_calls_cap = new_cap;
            }
        }

        if (buf->tool_calls_count < buf->tool_calls_cap)
        {
            ToolCall *dst = &buf->tool_calls[buf->tool_calls_count];
            dst->name = str_dup(tname && cJSON_IsString(tname)
                                  ? cJSON_GetStringValue(tname) : "");
            dst->id = str_dup("");
            if (args)
            {
                char *args_str = cJSON_PrintUnformatted(args);
                dst->arguments = args_str ? args_str : str_dup("");
            }
            else
            {
                dst->arguments = str_dup("");
            }
            if (dst->name && dst->id && dst->arguments && dst->name[0])
                buf->tool_calls_count++;
            else
            {
                free(dst->name);
                free(dst->id);
                free(dst->arguments);
                memset(dst, 0, sizeof(*dst));
            }
        }
    }
}

/* ---------- Tests ---------- */

START_TEST(test_parse_single_tool_call)
{
    strdup_call_count = 0;
    strdup_fail_at = -1;

    WriteBuf buf = {0};
    cJSON *json = cJSON_Parse(
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"bash\",\"arguments\":{\"cmd\":\"ls\"}}}"
        "]}}");
    ck_assert_ptr_ne(json, NULL);

    cJSON *msg = cJSON_GetObjectItem(json, "message");
    ck_assert_ptr_ne(msg, NULL);

    parse_stream_tool_calls(&buf, msg);

    ck_assert_int_eq(buf.tool_calls_count, 1);
    ck_assert_int_eq(buf.tool_calls_cap, 4);
    ck_assert_str_eq(buf.tool_calls[0].name, "bash");
    ck_assert_ptr_ne(buf.tool_calls[0].arguments, NULL);

    tool_call_free_mirror(&buf.tool_calls[0]);
    free(buf.tool_calls);
    cJSON_Delete(json);
}
END_TEST

START_TEST(test_parse_multiple_tool_calls)
{
    strdup_call_count = 0;
    strdup_fail_at = -1;

    WriteBuf buf = {0};
    cJSON *json = cJSON_Parse(
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"bash\",\"arguments\":{\"cmd\":\"ls\"}}},"
        "{\"function\":{\"name\":\"read_file\",\"arguments\":{\"file\":\"x\"}}}"
        "]}}");
    ck_assert_ptr_ne(json, NULL);

    cJSON *msg = cJSON_GetObjectItem(json, "message");
    ck_assert_ptr_ne(msg, NULL);

    parse_stream_tool_calls(&buf, msg);

    ck_assert_int_eq(buf.tool_calls_count, 2);
    ck_assert_str_eq(buf.tool_calls[0].name, "bash");
    ck_assert_str_eq(buf.tool_calls[1].name, "read_file");

    for (int i = 0; i < buf.tool_calls_count; i++)
        tool_call_free_mirror(&buf.tool_calls[i]);
    free(buf.tool_calls);
    cJSON_Delete(json);
}
END_TEST

START_TEST(test_skip_tool_call_no_name)
{
    strdup_call_count = 0;
    strdup_fail_at = -1;

    WriteBuf buf = {0};
    cJSON *json = cJSON_Parse(
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"\",\"arguments\":{}}}"
        "]}}");
    ck_assert_ptr_ne(json, NULL);

    cJSON *msg = cJSON_GetObjectItem(json, "message");
    ck_assert_ptr_ne(msg, NULL);

    parse_stream_tool_calls(&buf, msg);

    ck_assert_int_eq(buf.tool_calls_count, 0);
    ck_assert_int_eq(buf.tool_calls_cap, 4);

    free(buf.tool_calls);
    cJSON_Delete(json);
}
END_TEST

START_TEST(test_no_tool_calls_field)
{
    strdup_call_count = 0;
    strdup_fail_at = -1;

    WriteBuf buf = {0};
    cJSON *json = cJSON_Parse(
        "{\"message\":{\"content\":\"hello\"}}");
    ck_assert_ptr_ne(json, NULL);

    cJSON *msg = cJSON_GetObjectItem(json, "message");
    ck_assert_ptr_ne(msg, NULL);

    parse_stream_tool_calls(&buf, msg);

    ck_assert_int_eq(buf.tool_calls_count, 0);
    ck_assert_ptr_eq(buf.tool_calls, NULL);

    cJSON_Delete(json);
}
END_TEST

START_TEST(test_alloc_fail_name)
{
    strdup_call_count = 0;
    strdup_fail_at = 1;

    WriteBuf buf = {0};
    cJSON *json = cJSON_Parse(
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"bash\",\"arguments\":{\"cmd\":\"ls\"}}}"
        "]}}");
    ck_assert_ptr_ne(json, NULL);

    cJSON *msg = cJSON_GetObjectItem(json, "message");
    ck_assert_ptr_ne(msg, NULL);

    parse_stream_tool_calls(&buf, msg);

    ck_assert_int_eq(buf.tool_calls_count, 0);
    ck_assert_int_eq(buf.tool_calls_cap, 4);
    ck_assert_ptr_eq(buf.tool_calls[0].name, NULL);

    free(buf.tool_calls);
    cJSON_Delete(json);
}
END_TEST

START_TEST(test_alloc_fail_id)
{
    strdup_call_count = 0;
    strdup_fail_at = 2;

    WriteBuf buf = {0};
    cJSON *json = cJSON_Parse(
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"bash\",\"arguments\":{\"cmd\":\"ls\"}}}"
        "]}}");
    ck_assert_ptr_ne(json, NULL);

    cJSON *msg = cJSON_GetObjectItem(json, "message");
    ck_assert_ptr_ne(msg, NULL);

    parse_stream_tool_calls(&buf, msg);

    ck_assert_int_eq(buf.tool_calls_count, 0);
    ck_assert_ptr_eq(buf.tool_calls[0].name, NULL);

    free(buf.tool_calls);
    cJSON_Delete(json);
}
END_TEST

START_TEST(test_alloc_fail_arguments)
{
    strdup_call_count = 0;
    strdup_fail_at = 3;

    WriteBuf buf = {0};
    /* missing arguments field → args is NULL → hits the else branch */
    cJSON *json = cJSON_Parse(
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"bash\"}}"
        "]}}");
    ck_assert_ptr_ne(json, NULL);

    cJSON *msg = cJSON_GetObjectItem(json, "message");
    ck_assert_ptr_ne(msg, NULL);

    parse_stream_tool_calls(&buf, msg);

    ck_assert_int_eq(buf.tool_calls_count, 0);
    ck_assert_ptr_eq(buf.tool_calls[0].name, NULL);
    ck_assert_ptr_eq(buf.tool_calls[0].id, NULL);

    free(buf.tool_calls);
    cJSON_Delete(json);
}
END_TEST

START_TEST(test_recovery_after_failure)
{
    strdup_call_count = 0;
    strdup_fail_at = -1;

    WriteBuf buf = {0};
    cJSON *json = cJSON_Parse(
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"bash\",\"arguments\":{\"cmd\":\"ls\"}}}"
        "]}}");
    ck_assert_ptr_ne(json, NULL);

    cJSON *msg = cJSON_GetObjectItem(json, "message");
    ck_assert_ptr_ne(msg, NULL);

    parse_stream_tool_calls(&buf, msg);
    ck_assert_int_eq(buf.tool_calls_count, 1);

    parse_stream_tool_calls(&buf, msg);
    ck_assert_int_eq(buf.tool_calls_count, 2);

    ck_assert_str_eq(buf.tool_calls[0].name, "bash");
    ck_assert_str_eq(buf.tool_calls[1].name, "bash");

    for (int i = 0; i < buf.tool_calls_count; i++)
        tool_call_free_mirror(&buf.tool_calls[i]);
    free(buf.tool_calls);
    cJSON_Delete(json);
}
END_TEST

START_TEST(test_capacity_growth)
{
    strdup_call_count = 0;
    strdup_fail_at = -1;

    WriteBuf buf = {0};
    /* 6 tool calls to force cap 4 -> 8 growth */
    const char *json_str =
        "{\"message\":{\"tool_calls\":["
        "{\"function\":{\"name\":\"a\",\"arguments\":{\"k\":\"v\"}}},"
        "{\"function\":{\"name\":\"b\",\"arguments\":{\"k\":\"v\"}}},"
        "{\"function\":{\"name\":\"c\",\"arguments\":{\"k\":\"v\"}}},"
        "{\"function\":{\"name\":\"d\",\"arguments\":{\"k\":\"v\"}}},"
        "{\"function\":{\"name\":\"e\",\"arguments\":{\"k\":\"v\"}}},"
        "{\"function\":{\"name\":\"f\",\"arguments\":{\"k\":\"v\"}}}"
        "]}}";
    cJSON *json = cJSON_Parse(json_str);
    ck_assert_ptr_ne(json, NULL);

    cJSON *msg = cJSON_GetObjectItem(json, "message");
    ck_assert_ptr_ne(msg, NULL);

    parse_stream_tool_calls(&buf, msg);

    ck_assert_int_eq(buf.tool_calls_count, 6);
    ck_assert_int_eq(buf.tool_calls_cap, 8);
    ck_assert_str_eq(buf.tool_calls[5].name, "f");

    for (int i = 0; i < buf.tool_calls_count; i++)
        tool_call_free_mirror(&buf.tool_calls[i]);
    free(buf.tool_calls);
    cJSON_Delete(json);
}
END_TEST

Suite *ollama_tool_calls_suite(void)
{
    Suite *s = suite_create("ollama_tool_calls");
    TCase *tc = tcase_create("parsing");

    tcase_add_test(tc, test_parse_single_tool_call);
    tcase_add_test(tc, test_parse_multiple_tool_calls);
    tcase_add_test(tc, test_skip_tool_call_no_name);
    tcase_add_test(tc, test_no_tool_calls_field);
    tcase_add_test(tc, test_alloc_fail_name);
    tcase_add_test(tc, test_alloc_fail_id);
    tcase_add_test(tc, test_alloc_fail_arguments);
    tcase_add_test(tc, test_recovery_after_failure);
    tcase_add_test(tc, test_capacity_growth);
    suite_add_tcase(s, tc);

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
