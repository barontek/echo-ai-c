#include "utils/html_extract.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Fuzz target for html_extract_text: the parser must never crash, overrun,
 * or allocate unboundedly regardless of input. */
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    /* Keep allocation failure injection disabled during fuzzing. */
    char *out = html_extract_text((const char *)data, size,
                                  size % 1000);
    free(out);
    return 0;
}
