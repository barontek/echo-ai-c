/* fuzz_fernet_token - libFuzzer target for fernet token parsing. Depends on: check, the module under test. */
/* libFuzzer harness for the Fernet session-token layout parser in
 * src/session/encryption.c (decrypt_fernet_token via
 * encryption_test_validate_token under ENCRYPTION_TEST). Fuzzes raw
 * token bytes: version byte, minimum length, and the IV/ciphertext/HMAC
 * offset arithmetic.
 *
 * Build with: -fsanitize=fuzzer,address,undefined
 * Run with:    ./fuzz_fernet_token -max_len=4096
 */

#include <stddef.h>
#include <stdint.h>

int encryption_test_validate_token(const unsigned char *token, int token_len);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 1024U * 1024U) return 0;
    (void)encryption_test_validate_token(data, size);
    return 0;
}
