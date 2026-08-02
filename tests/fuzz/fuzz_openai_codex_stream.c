#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "llm/openai.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 1024U * 1024U || memchr(data, '\0', size)) return 0;
    char *stream = malloc(size + 1);
    if (!stream) return 0;
    memcpy(stream, data, size);
    stream[size] = '\0';
    size_t first = size / 3;
    size_t second = (size - first) / 2;
    const char *fragments[] = {stream, stream + first, stream + first + second};
    size_t lengths[] = {first, second, size - first - second};
    LLMResponse *response = openai_test_stream_fragments(
        fragments, lengths, 3, NULL, NULL);
    llm_response_free(response);
    free(stream);
    return 0;
}
