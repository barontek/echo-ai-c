#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "agent/message.h"

LLMResponse *openai_compatible_test_parse_stream_alloc(
    const char *input, void (*on_chunk)(const char *, void *), void *userdata);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 1024U * 1024U || memchr(data, '\0', size)) return 0;
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';
    LLMResponse *resp = openai_compatible_test_parse_stream_alloc(input, NULL, NULL);
    llm_response_free(resp);
    free(input);
    return 0;
}
