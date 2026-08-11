/* fuzz_openai_parse_models - libFuzzer target for openai parse models parsing. Depends on: check, the module under test. */
/* libFuzzer harness for parse_models_response — the Codex models-catalog
 * parser in src/llm/openai.c. Compiles the REAL openai.c under
 * OPENAI_TEST and drives the existing openai_test_parse_models hook.
 *
 * Build with: -fsanitize=fuzzer,address,undefined
 * Run with:    ./fuzz_openai_parse_models -max_len=4096
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int openai_test_parse_models(const char *raw, char ***models_out,
                             size_t *count_out);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 1024U * 1024U || memchr(data, '\0', size)) return 0;
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';
    char **models = NULL;
    size_t count = 0;
    if (openai_test_parse_models(input, &models, &count) == 0 && models)
    {
        for (size_t i = 0; i < count; i++)
            free(models[i]);
        free(models);
    }
    free(input);
    return 0;
}
