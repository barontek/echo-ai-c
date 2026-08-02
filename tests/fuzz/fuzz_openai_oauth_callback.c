#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "llm/openai_oauth.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *code = NULL;
    char *state = NULL;
    char *denial = NULL;
    (void)openai_oauth_test_parse_callback(data, size, &code, &state, &denial);
    free(code);
    free(state);
    free(denial);
    return 0;
}
