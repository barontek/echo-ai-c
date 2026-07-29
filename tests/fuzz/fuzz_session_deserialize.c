/* libFuzzer harness for session_deserialize_messages — the JSON message
 * array parser that deserializes conversation history from the database.
 *
 * Feeds null-terminated fuzz data directly to the deserializer, which
 * allocates and populates Message structs from the JSON.
 *
 * Build with: -fsanitize=fuzzer,address,undefined
 * Run with:    ./fuzz_session_deserialize -max_len=16384
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "session/session.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) return 0;
    if (size > 65536) return 0;

    /* Null-terminate for cJSON parser. */
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    Session *s = session_create(NULL);
    if (!s)
    {
        free(input);
        return 0;
    }

    session_deserialize_messages(s, input);
    session_free(s);
    free(input);

    return 0;
}
