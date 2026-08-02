#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "llm/openai_oauth.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 1024U * 1024U || memchr(data, '\0', size)) return 0;
    char *json = malloc(size + 1);
    if (!json) return 0;
    memcpy(json, data, size);
    json[size] = '\0';
    char *access = NULL;
    char *refresh = NULL;
    char *account = NULL;
    char *plan = NULL;
    time_t expires_at = 0;
    (void)openai_oauth_test_parse_token(json, 0, 1, &access, &refresh,
                                        &account, &plan, &expires_at);
    free(access);
    free(refresh);
    free(account);
    free(plan);
    free(json);
    return 0;
}
