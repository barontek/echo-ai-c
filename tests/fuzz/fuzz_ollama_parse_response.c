/* libFuzzer harness for ollama_parse_response — the non-stream response
 * parser for the Ollama /api/chat endpoint. Compiles the REAL
 * src/llm/ollama.c under OLLAMA_TEST and drives the existing
 * ollama_test_parse_response hook (no mirror).
 *
 * Build with: -fsanitize=fuzzer,address,undefined
 * Run with:    ./fuzz_ollama_parse_response -max_len=4096
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
    LLMResponse *resp = ollama_test_parse_response(input);
    llm_response_free(resp);
    free(input);
    return 0;
}
