/*
 * encryption.c - Fernet-based field-level encryption for session records:
 * scrypt key derivation, AES-128-CBC encrypt/decrypt with HMAC-SHA256
 * signing, and password/salt/verifier file handling.
 * Depends on: openssl (evp, hmac, rand, err), logging, string_utils.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>

#include <arpa/inet.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/crypto.h>

#include "encryption.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

#define SCRYPT_N 262144
#define SCRYPT_R 8
#define SCRYPT_P 1
#define SALT_SIZE 16
#define PEPPER_SIZE 32
#define SALT_PEPPER_MAX 64
#define FERNET_VERSION 0x80
#define IV_SIZE 16
#define HMAC_SIZE 32
#define KEY_ENCRYPTION_HALF 16
#define KEY_SIGNING_HALF 16

int encryption_key_derive(const char *password, const unsigned char *salt, int salt_len,
                          const unsigned char *pepper, int pepper_len,
                          EncryptionKey *key)
{
    if (!password || !salt || salt_len <= 0 || !key) return -1;
    if (pepper && (pepper_len <= 0 || pepper_len > SALT_PEPPER_MAX)) return -1;
    if (salt_len > SALT_PEPPER_MAX || (pepper && salt_len + pepper_len > SALT_PEPPER_MAX * 2))
        return -1;

    /* Pepper mixing: scrypt runs over salt||pepper so the derived key
     * depends on a secret held only on the machine that created the
     * vault. A leaked copy of the DB (salt + verifier + tokens) can no
     * longer be used as an offline password-testing oracle. */
    unsigned char salt_combined[SALT_PEPPER_MAX * 2];
    int combined_len = salt_len;
    memcpy(salt_combined, salt, (size_t)salt_len);
    if (pepper && pepper_len > 0)
    {
        if (salt_len + pepper_len > (int)sizeof(salt_combined)) return -1;
        memcpy(salt_combined + salt_len, pepper, (size_t)pepper_len);
        combined_len += pepper_len;
    }

    int rc = EVP_PBE_scrypt(password, strlen(password), salt_combined, combined_len,
                            SCRYPT_N, SCRYPT_R, SCRYPT_P, (uint64_t)512 * 1024 * 1024,
                            key->key, sizeof(key->key));
    if (rc != 1)
    {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        log_error("EVP_PBE_scrypt failed", "err", err_buf, NULL);
        return -1;
    }

    memset(salt_combined, 0, sizeof(salt_combined));
    return 0;
}

/* Open a vault key-material file for exclusive creation with 0600 mode
 * (salt, pepper, verifier). The umask-independent mode matters: these
 * files sit next to the DB and must not be world-readable. Returns a
 * FILE* over the fd, or NULL (unlinking on failure so no partial file
 * survives). */
static FILE *open_secure_excl(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f)
    {
        close(fd);
        (void)unlink(path);
        return NULL;
    }
    return f;
}

static int build_fernet_token(const unsigned char *aes_key, const unsigned char *hmac_key,
                              const unsigned char *plaintext, int plaintext_len,
                              unsigned char **out_token, int *out_len)
{
    unsigned char iv[IV_SIZE];
    if (RAND_bytes(iv, IV_SIZE) != 1)
    {
        log_error("RAND_bytes failed", NULL);
        return -1;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int ciphertext_len = 0;
    int len = 0;
    unsigned char *ciphertext = calloc(1, plaintext_len + 16);
    if (!ciphertext) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, aes_key, iv);
    EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len);
    ciphertext_len = len;
    EVP_EncryptFinal_ex(ctx, ciphertext + len, &len);
    ciphertext_len += len;
    EVP_CIPHER_CTX_free(ctx);

    uint64_t timestamp_be = (uint64_t)time(NULL);
    /* htonl dance converts the 64-bit unix timestamp to big-endian byte
     * order (Fernet layout); timestamp 0 is invalid per the Fernet spec,
     * so clamp it to 1. */
    timestamp_be = ((uint64_t)htonl((uint32_t)(timestamp_be >> 32))) |
                   (((uint64_t)htonl((uint32_t)timestamp_be)) << 32);
    if (timestamp_be == 0) timestamp_be = 1;

    int token_size = 1 + 8 + IV_SIZE + ciphertext_len + HMAC_SIZE;
    unsigned char *token = malloc(token_size);
    if (!token) {
        free(ciphertext);
        return -1;
    }

    int pos = 0;
    token[pos++] = FERNET_VERSION;
    memcpy(token + pos, &timestamp_be, 8);
    pos += 8;
    memcpy(token + pos, iv, IV_SIZE);
    pos += IV_SIZE;
    memcpy(token + pos, ciphertext, ciphertext_len);
    pos += ciphertext_len;

    unsigned char hmac[HMAC_SIZE];
    unsigned int hmac_len = HMAC_SIZE;
    HMAC(EVP_sha256(), hmac_key, KEY_SIGNING_HALF, token, pos, hmac, &hmac_len);
    memcpy(token + pos, hmac, HMAC_SIZE);
    pos += HMAC_SIZE;

    free(ciphertext);
    *out_token = token;
    *out_len = pos;
    return 0;
}

static int decrypt_fernet_token(const unsigned char *aes_key, const unsigned char *hmac_key,
                                const unsigned char *token, int token_len,
                                unsigned char **out_plaintext, int *out_len)
{
    if (token_len < 1 + 8 + IV_SIZE + 1 + HMAC_SIZE) return -1;
    if (token[0] != FERNET_VERSION) return -1;

    int pos = 1;
    /* Fernet spec freshness rule: reject tokens timestamped more than 60
     * seconds in the future (clock-skew tolerance). Past timestamps are
     * always accepted — this vault has no TTL semantics, so tokens keep
     * their full lifetime, but a token minted "ahead of now" is evidence
     * of a forged or replayed clock and is refused. */
    {
        uint64_t ts_be = 0;
        memcpy(&ts_be, token + pos, 8);
        uint64_t ts = ((uint64_t)ntohl((uint32_t)(ts_be >> 32))) |
                      (((uint64_t)ntohl((uint32_t)ts_be)) << 32);
        if (ts != 0 && (int64_t)ts > (int64_t)time(NULL) + 60) return -1;
    }
    pos += 8;
    const unsigned char *iv = token + pos;
    pos += IV_SIZE;
    const unsigned char *ciphertext = token + pos;
    int ciphertext_len = token_len - pos - HMAC_SIZE;
    const unsigned char *stored_hmac = token + pos + ciphertext_len;

    unsigned char computed_hmac[HMAC_SIZE];
    unsigned int hmac_len = HMAC_SIZE;
    HMAC(EVP_sha256(), hmac_key, KEY_SIGNING_HALF, token, pos + ciphertext_len, computed_hmac, &hmac_len);

    /* Constant-time compare (CRYPTO_memcmp, same primitive as the unlock
     * token check in middleware.c): a plain memcmp short-circuits at the
     * first differing byte and turns the HMAC check into a byte-wise
     * timing oracle for the signing key. */
    if (CRYPTO_memcmp(computed_hmac, stored_hmac, HMAC_SIZE) != 0) return -1;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    unsigned char *plaintext = malloc(ciphertext_len + 16);
    if (!plaintext) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    /* Every EVP call is checked: DecryptFinal_ex fails on bad padding, and
     * on failure the plaintext region beyond the bytes DecryptUpdate wrote
     * is unwritten — handing that to callers (which NUL-terminate and
     * return it) is the classic EVP trap. Error => no output at all. */
    int plaintext_len = 0;
    int len = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, aes_key, iv) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        free(plaintext);
        return -1;
    }
    if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        free(plaintext);
        return -1;
    }
    plaintext_len = len;
    if (EVP_DecryptFinal_ex(ctx, plaintext + len, &len) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        free(plaintext);
        return -1;
    }
    plaintext_len += len;
    EVP_CIPHER_CTX_free(ctx);

    plaintext[plaintext_len] = '\0';
    *out_plaintext = plaintext;
    *out_len = plaintext_len;
    return 0;
}

#ifdef ENCRYPTION_TEST
/* Test-only fault-injection seam (AGENTS.md "Fault-injection testing"):
 * force fclose to fail at a chosen call so the salt/pepper/verifier store
 * paths prove they surface a failed close (a close after a checked fwrite
 * is where a silently-lost flush hides) instead of returning success. */
static int test_fclose_fail_at = -1;
static int test_fclose_call_count = 0;
static int test_fclose(FILE *fp)
{
    test_fclose_call_count++;
    if (test_fclose_call_count == test_fclose_fail_at) return EOF;
    return fclose(fp);
}
#define fclose test_fclose
void encryption_test_set_fclose_fail(int nth_close)
{
    test_fclose_fail_at = nth_close;
    test_fclose_call_count = 0;
}

/* Structural validation of the real Fernet-token parse path over
 * arbitrary bytes (version byte, minimum length, layout arithmetic,
 * HMAC comparison) using fixed zero keys — the fuzzer cannot forge a
 * matching HMAC, so the decrypt branch only runs on genuinely valid
 * tokens, while the bounds logic is exercised on everything else.
 * Returns 1 when the token parses, 0 when it is rejected. */
int encryption_test_validate_token(const unsigned char *token, int token_len)
{
    if (!token || token_len <= 0) return 0;
    unsigned char aes_key[KEY_SIGNING_HALF] = {0};
    unsigned char hmac_key[KEY_SIGNING_HALF] = {0};
    unsigned char *plaintext = NULL;
    int plaintext_len = 0;
    int rc = decrypt_fernet_token(aes_key, hmac_key, token, token_len,
                                  &plaintext, &plaintext_len);
    if (rc == 0)
    {
        free(plaintext);
        return 1;
    }
    return 0;
}
#endif

unsigned char *encryption_encrypt(const EncryptionKey *key, const unsigned char *plaintext, int plaintext_len, int *out_len)
{
    if (!key || !plaintext || plaintext_len <= 0 || !out_len) return NULL;

    const unsigned char *aes_key = key->key + KEY_SIGNING_HALF;
    const unsigned char *hmac_key = key->key;

    unsigned char *token = NULL;
    int token_len = 0;
    if (build_fernet_token(aes_key, hmac_key, plaintext, plaintext_len, &token, &token_len) != 0)
        return NULL;

    *out_len = token_len;
    return token;
}

unsigned char *encryption_decrypt(const EncryptionKey *key, const unsigned char *token, int token_len, int *out_len)
{
    if (!key || !token || token_len <= 0 || !out_len) return NULL;

    const unsigned char *aes_key = key->key + KEY_SIGNING_HALF;
    const unsigned char *hmac_key = key->key;

    unsigned char *plaintext = NULL;
    int plaintext_len = 0;
    if (decrypt_fernet_token(aes_key, hmac_key, token, token_len, &plaintext, &plaintext_len) != 0)
        return NULL;

    *out_len = plaintext_len;
    return plaintext;
}

int encryption_salt_create(const char *salt_path)
{
    unsigned char salt[SALT_SIZE];
    if (RAND_bytes(salt, SALT_SIZE) != 1)
    {
        log_error("RAND_bytes failed for salt", NULL);
        return -1;
    }

    FILE *f = open_secure_excl(salt_path);
    if (!f)
    {
        log_error("failed to create salt file", "path", salt_path, NULL);
        return -1;
    }

    if (fwrite(salt, 1, SALT_SIZE, f) != SALT_SIZE)
    {
        log_error("failed to write salt", NULL);
        fclose(f); // NOLINT(cert-err33-c)
        unlink(salt_path);
        return -1;
    }

    if (fclose(f) != 0)
    {
        log_error("failed to flush salt file", NULL); // NOLINT(clang-analyzer-unix.Stream)
        unlink(salt_path);
        memset(salt, 0, sizeof(salt));
        return -1;
    }
    memset(salt, 0, sizeof(salt));
    return 0;
}

int encryption_pepper_create(const char *pepper_path)
{
    unsigned char pepper[PEPPER_SIZE];
    if (RAND_bytes(pepper, PEPPER_SIZE) != 1)
    {
        log_error("RAND_bytes failed for pepper", NULL);
        return -1;
    }

    FILE *f = open_secure_excl(pepper_path);
    if (!f)
    {
        log_error("failed to create pepper file", "path", pepper_path, NULL);
        return -1;
    }

    if (fwrite(pepper, 1, PEPPER_SIZE, f) != PEPPER_SIZE)
    {
        log_error("failed to write pepper", NULL);
        fclose(f); // NOLINT(cert-err33-c)
        unlink(pepper_path);
        return -1;
    }

    if (fclose(f) != 0)
    {
        log_error("failed to flush pepper file", NULL); // NOLINT(clang-analyzer-unix.Stream)
        unlink(pepper_path);
        memset(pepper, 0, sizeof(pepper));
        return -1;
    }
    memset(pepper, 0, sizeof(pepper));
    return 0;
}

int encryption_salt_load(const char *salt_path, unsigned char *salt, int *salt_len)
{
    FILE *f = fopen(salt_path, "rb");
    if (!f) return -1;

    long file_size;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f); // NOLINT(cert-err33-c)
        return -1;
    }
    file_size = ftell(f);
    if (file_size < 0 || file_size > 64)
    {
        fclose(f); // NOLINT(cert-err33-c)
        return -1; // NOLINT(clang-analyzer-unix.Stream)
      }
      if (fseek(f, 0, SEEK_SET) != 0)
      {
          fclose(f); // NOLINT(cert-err33-c)
          return -1;
      }

      size_t read = fread(salt, 1, file_size, f);
    fclose(f); // NOLINT(cert-err33-c)

    if (read != (size_t)file_size) return -1;
    *salt_len = (int)read;
    return 0;
}

int encryption_pepper_load(const char *pepper_path, unsigned char *pepper, int *pepper_len)
{
    FILE *f = fopen(pepper_path, "rb");
    if (!f) return -1;

    long file_size;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f); // NOLINT(cert-err33-c)
        return -1;
    }
    file_size = ftell(f);
    if (file_size < 0 || file_size > 64)
    {
        fclose(f); // NOLINT(cert-err33-c)
        return -1; // NOLINT(clang-analyzer-unix.Stream)
    }
    if (fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f); // NOLINT(cert-err33-c)
        return -1;
    }

    size_t read = fread(pepper, 1, file_size, f);
    fclose(f); // NOLINT(cert-err33-c)

    if (read != (size_t)file_size) return -1;
    *pepper_len = (int)read;
    return 0;
}

int encryption_first_run_detect(const char *data_dir)
{
    char *salt_path = NULL;
    if (asprintf(&salt_path, "%s/salt", data_dir) < 0) return 0;

    struct stat st;
    int salt_exists = (stat(salt_path, &st) == 0);
    free(salt_path);

    if (!salt_exists) return 1;
    return 0;
}

int encryption_create_verifier(const EncryptionKey *key, const char *path)
{
    const char *plaintext = "echo-ai-ok";
    int out_len = 0;
    unsigned char *token = encryption_encrypt(key, (const unsigned char *)plaintext, strlen(plaintext), &out_len);
    if (!token) return -1;

    FILE *f = open_secure_excl(path);
    if (!f) {
        free(token);
        return -1;
    }

    if (fwrite(token, 1, out_len, f) != (size_t)out_len)
    {
        fclose(f); // NOLINT(cert-err33-c)
        unlink(path);
        free(token);
        return -1;
    }
    if (fclose(f) != 0)
    {
        unlink(path); // NOLINT(clang-analyzer-unix.Stream)
        free(token);
        return -1;
    }
    free(token);
    return 0;
}

int encryption_check_verifier(const EncryptionKey *key, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END); // NOLINT(cert-err33-c)
long fsize = ftell(f);
      if (fsize <= 0 || fsize > 4096 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); // NOLINT(cert-err33-c)
        return -1; // NOLINT(clang-analyzer-unix.Stream)
    }

    unsigned char *data = malloc((size_t)fsize);
    if (!data) {
        fclose(f); // NOLINT(cert-err33-c)
        return -1;
    }

    size_t read = fread(data, 1, (size_t)fsize, f);
    fclose(f); // NOLINT(cert-err33-c)
    if (read != (size_t)fsize) {
        free(data);
        return -1;
    }

    int out_len = 0;
    unsigned char *dec = encryption_decrypt(key, data, (int)read, &out_len);
    free(data);

    if (!dec) return -1;

    int match = (out_len == 10 && memcmp(dec, "echo-ai-ok", 10) == 0) ? 0 : -1;
    free(dec);
    return match;
}
