/* fuzz_tool_args - libFuzzer target for tool_args_compact, the tool-call
 * arguments summarizer. Model-provided JSON is external input: malformed
 * trees, huge values, and hostile UTF-8 must never crash or overrun the
 * cap.
 *
 * Build with: -fsanitize=fuzzer,address,undefined
 * Run with:   ./fuzz_tool_args -max_len=4096
 */

#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tui/tool_args.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) return 0;
    if (size > 65536) return 0;

    /* Exercise several cap sizes, including the degenerate ones. */
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    size_t caps[] = {1, 2, 3, 7, 64, 200, 4096};
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++)
    {
        char *out = tool_args_compact(input, caps[i]);
        if (out)
        {
            if (strlen(out) > caps[i]) abort(); /* cap violated */
            free(out);
        }
    }
    free(input);
    return 0;
}
