/* libFuzzer harness for parse_stream_tool_calls — the JSON tool_calls
 * parser that processes LLM streaming output.  Uses the same mirror approach
 * as test_ollama_tool_calls.c: the function under test is a static in
 * src/llm/ollama.c, so we replicate it here.  Keep in sync with ollama.c.
 *
 * Build with: -fsanitize=fuzzer,address,undefined
 * Run with:    ./fuzz_ollama_tool_calls -max_len=4096
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

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

static char *fuzz_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void fuzz_tool_call_free(ToolCall *call)
{
    if (!call) return;
    free(call->id);
    free(call->name);
    free(call->arguments);
}

/* Mirrors parse_stream_tool_calls from src/llm/ollama.c — keep in sync. */
static void fuzz_parse_tool_calls(WriteBuf *buf, cJSON *msg)
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
            if (!new_tc) break; /* OOM — stop parsing gracefully */
            memset(new_tc + buf->tool_calls_cap, 0,
                   sizeof(ToolCall) * (size_t)(new_cap - buf->tool_calls_cap));
            buf->tool_calls = new_tc;
            buf->tool_calls_cap = new_cap;
        }

        if (buf->tool_calls_count < buf->tool_calls_cap)
        {
            ToolCall *dst = &buf->tool_calls[buf->tool_calls_count];
            dst->name = fuzz_strdup(tname && cJSON_IsString(tname)
                                      ? cJSON_GetStringValue(tname) : "");
            dst->id = fuzz_strdup("");
            if (args)
            {
                char *args_str = cJSON_PrintUnformatted(args);
                dst->arguments = args_str ? args_str : fuzz_strdup("");
            }
            else
            {
                dst->arguments = fuzz_strdup("");
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

static void fuzz_writebuf_cleanup(WriteBuf *buf)
{
    for (int i = 0; i < buf->tool_calls_count; i++)
        fuzz_tool_call_free(&buf->tool_calls[i]);
    free(buf->tool_calls);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) return 0;
    if (size > 65536) return 0;

    /* Null-terminate the fuzz input for cJSON */
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    cJSON *root = cJSON_Parse(input);
    free(input);
    if (!root) return 0;

    /* Parse top-level JSON in two styles: {"tool_calls":[...]} (direct)
     * and {"message":{"tool_calls":[...]}} (streaming chunk wrapper) */
    WriteBuf buf1 = {0};
    WriteBuf buf2 = {0};

    fuzz_parse_tool_calls(&buf1, root);

    cJSON *msg = cJSON_GetObjectItem(root, "message");
    if (msg) fuzz_parse_tool_calls(&buf2, msg);

    fuzz_writebuf_cleanup(&buf1);
    fuzz_writebuf_cleanup(&buf2);
    cJSON_Delete(root);

    return 0;
}
