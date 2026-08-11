/* fuzz_websocket_frame - libFuzzer target for websocket frame parsing. Depends on: check, the module under test. */
/* libFuzzer harness for the RFC 6455 frame parser in
 * src/server/websocket.c (ws_frame_header, the same code the read loop
 * uses — exposed to tests via websocket_test_frame_walk under
 * WEBSOCKET_TEST). Fuzzes raw network bytes.
 *
 * Build with: -fsanitize=fuzzer,address,undefined
 * Run with:    ./fuzz_websocket_frame -max_len=4096
 */

#include <stddef.h>
#include <stdint.h>

size_t websocket_test_frame_walk(const unsigned char *data, size_t len);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 1024U * 1024U) return 0;
    (void)websocket_test_frame_walk(data, size);
    return 0;
}
