#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "llm/openai_oauth.h"

/* fuzz_openai_oauth_jwt - libFuzzer target for openai oauth jwt parsing. Depends on: check, the module under test. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 1024U * 1024U || memchr(data, '\0', size)) return 0;
    char *jwt = malloc(size + 1);
    if (!jwt) return 0;
    memcpy(jwt, data, size);
    jwt[size] = '\0';
    char *account = NULL;
    char *plan = NULL;
    (void)openai_oauth_test_jwt_metadata(jwt, &account, &plan);
    free(account);
    free(plan);
    free(jwt);
    return 0;
}
