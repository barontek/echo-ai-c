#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <arpa/inet.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/err.h>

#include "encryption.h"
#include "../utils/logging.h"
#include "../utils/string_utils.h"

#define SCRYPT_N 262144
#define SCRYPT_R 8
#define SCRYPT_P 1
#define SALT_SIZE 16
#define FERNET_VERSION 0x80
#define IV_SIZE 16
#define HMAC_SIZE 32
#define KEY_ENCRYPTION_HALF 16
#define KEY_SIGNING_HALF 16

int encryption_key_derive(const char *password, const unsigned char *salt, int salt_len, EncryptionKey *key)
{
    if (!password || !salt || salt_len <= 0 || !key) return -1;

    int rc = EVP_PBE_scrypt(password, strlen(password), salt, salt_len,
                            SCRYPT_N, SCRYPT_R, SCRYPT_P, 512 * 1024 * 1024,
                            key->key, sizeof(key->key));
    if (rc != 1)
    {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        log_error("EVP_PBE_scrypt failed", "err", err_buf, NULL);
        return -1;
    }

    return 0;
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

    int ciphertext_len = 0, len = 0;
    unsigned char *ciphertext = calloc(1, plaintext_len + 16);
    if (!ciphertext) { EVP_CIPHER_CTX_free(ctx); return -1; }

    EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, aes_key, iv);
    EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len);
    ciphertext_len = len;
    EVP_EncryptFinal_ex(ctx, ciphertext + len, &len);
    ciphertext_len += len;
    EVP_CIPHER_CTX_free(ctx);

    uint64_t timestamp_be = (uint64_t)time(NULL);
    timestamp_be = ((uint64_t)htonl((uint32_t)(timestamp_be >> 32))) |
                   (((uint64_t)htonl((uint32_t)timestamp_be)) << 32);
    if (timestamp_be == 0) timestamp_be = 1;

    int token_size = 1 + 8 + IV_SIZE + ciphertext_len + HMAC_SIZE;
    unsigned char *token = malloc(token_size);
    if (!token) { free(ciphertext); return -1; }

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
    pos += 8;
    const unsigned char *iv = token + pos;
    pos += IV_SIZE;
    const unsigned char *ciphertext = token + pos;
    int ciphertext_len = token_len - pos - HMAC_SIZE;
    const unsigned char *stored_hmac = token + pos + ciphertext_len;

    unsigned char computed_hmac[HMAC_SIZE];
    unsigned int hmac_len = HMAC_SIZE;
    HMAC(EVP_sha256(), hmac_key, KEY_SIGNING_HALF, token, pos + ciphertext_len, computed_hmac, &hmac_len);

    if (memcmp(computed_hmac, stored_hmac, HMAC_SIZE) != 0) return -1;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    unsigned char *plaintext = malloc(ciphertext_len + 16);
    if (!plaintext) { EVP_CIPHER_CTX_free(ctx); return -1; }

    int plaintext_len = 0, len = 0;
    EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, aes_key, iv);
    EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len);
    plaintext_len = len;
    EVP_DecryptFinal_ex(ctx, plaintext + len, &len);
    plaintext_len += len;
    EVP_CIPHER_CTX_free(ctx);

    plaintext[plaintext_len] = '\0';
    *out_plaintext = plaintext;
    *out_len = plaintext_len;
    return 0;
}

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

char *encryption_resolve_password(void)
{
    const char *password = getenv("ECHO_PASSWORD");
    if (password) return str_dup(password);
    return NULL;
}

int encryption_salt_create(const char *salt_path)
{
    unsigned char salt[SALT_SIZE];
    if (RAND_bytes(salt, SALT_SIZE) != 1)
    {
        log_error("RAND_bytes failed for salt", NULL);
        return -1;
    }

    FILE *f = fopen(salt_path, "wbx");
    if (!f)
    {
        log_error("failed to create salt file", "path", salt_path, NULL);
        return -1;
    }

    if (fwrite(salt, 1, SALT_SIZE, f) != SALT_SIZE)
    {
        log_error("failed to write salt", NULL);
        fclose(f);
        unlink(salt_path);
        return -1;
    }

    fclose(f);
    return 0;
}

int encryption_salt_load(const char *salt_path, unsigned char *salt, int *salt_len)
{
    FILE *f = fopen(salt_path, "rb");
    if (!f) return -1;

    long file_size;
    if (fseek(f, 0, SEEK_END) != 0 || (file_size = ftell(f)) < 0 || file_size > 64)
    {
        fclose(f);
        return -1;
    }
    rewind(f);

    size_t read = fread(salt, 1, file_size, f);
    fclose(f);

    if (read != (size_t)file_size) return -1;
    *salt_len = (int)read;
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

    FILE *f = fopen(path, "wbx");
    if (!f) { free(token); return -1; }

    int rc = -1;
    if (fwrite(token, 1, out_len, f) == (size_t)out_len)
        rc = 0;
    fclose(f);
    free(token);
    return rc;
}

int encryption_check_verifier(const EncryptionKey *key, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    if (fsize <= 0 || fsize > 4096) { fclose(f); return -1; }

    unsigned char *data = malloc((size_t)fsize);
    if (!data) { fclose(f); return -1; }

    size_t read = fread(data, 1, (size_t)fsize, f);
    fclose(f);
    if (read != (size_t)fsize) { free(data); return -1; }

    int out_len = 0;
    unsigned char *dec = encryption_decrypt(key, data, (int)read, &out_len);
    free(data);

    if (!dec) return -1;

    int match = (out_len == 10 && memcmp(dec, "echo-ai-ok", 10) == 0) ? 0 : -1;
    free(dec);
    return match;
}
