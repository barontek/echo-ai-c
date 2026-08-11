/* fuzz_duckduckgo_html_extract - libFuzzer target for duckduckgo html extract parsing. Depends on: check, the module under test. */
/* libFuzzer harness for the DuckDuckGo results-page HTML extractor in
 * src/tools/search_duckduckgo.c (the real html_extract, exposed via
 * search_duckduckgo_test_extract under SEARCH_DUCKDUCKGO_TEST). Fuzzes
 * raw network HTML.
 *
 * Build with: -fsanitize=fuzzer,address,undefined
 * Run with:    ./fuzz_duckduckgo_html_extract -max_len=4096
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

char *search_duckduckgo_test_extract(const char *raw_html);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 1024U * 1024U || memchr(data, '\0', size)) return 0;
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';
    char *result = search_duckduckgo_test_extract(input);
    free(result);
    free(input);
    return 0;
}
