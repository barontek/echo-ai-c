/* fuzz_openai_parse_response - libFuzzer target for openai parse response parsing. Depends on: check, the module under test. */
/* libFuzzer harness for parse_response — the non-streaming Codex
 * response parser in src/llm/openai.c. Compiles the REAL openai.c under
 * OPENAI_TEST and drives the existing openai_test_parse_response_alloc hook.
 *
 * Build with: -fsanitize=fuzzer,address,undefined
 * Run with:    ./fuzz_openai_parse_response -max_len=4096
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "agent/message.h"

LLMResponse *openai_test_parse_response_alloc(const char *raw);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 1024U * 1024U || memchr(data, '\0', size)) return 0;
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';
    LLMResponse *resp = openai_test_parse_response_alloc(input);
    llm_response_free(resp);
    free(input);
    return 0;
}
