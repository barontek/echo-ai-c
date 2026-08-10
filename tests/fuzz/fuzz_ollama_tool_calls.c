/* libFuzzer harness for parse_stream_tool_calls — the JSON tool_calls
 * parser that processes LLM streaming output. Compiles the REAL
 * src/llm/ollama.c under OLLAMA_TEST and drives it through
 * ollama_test_parse_stream_calls_json. (This used to replicate the
 * parser by hand; mirrors drift, so the harness must exercise the real
 * code — see ollama.c's keep-old-capacity realloc semantics.)
 *
 * Build with: -fsanitize=fuzzer,address,undefined
 * Run with:    ./fuzz_ollama_tool_calls -max_len=4096
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "llm/ollama.h"


int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 1024U * 1024U || memchr(data, '\0', size)) return 0;
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';
    (void)ollama_test_parse_stream_calls_json(input);
    free(input);
    return 0;
}
