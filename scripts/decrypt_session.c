#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <arpa/inet.h>

#define SCRYPT_N 262144
#define SCRYPT_R 8
#define SCRYPT_P 1
#define FERNET_VERSION 0x80
#define IV_SIZE 16
#define HMAC_SIZE 32
#define KEY_SIZE 32
#define KEY_SIGNING_HALF 16

static int load_file(const char *path, unsigned char **out, int *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    *out = malloc(sz);
    if (!*out) { fclose(f); return -1; }
    if (fread(*out, 1, sz, f) != (size_t)sz) { free(*out); fclose(f); return -1; }
    *out_len = (int)sz;
    fclose(f);
    return 0;
}

static int do_decrypt(const unsigned char *aes_key, const unsigned char *hmac_key,
                      const unsigned char *token, int token_len,
                      unsigned char **out, int *out_len)
{
    if (token_len < 1 + 8 + IV_SIZE + 1 + HMAC_SIZE) return -1;
    if (token[0] != FERNET_VERSION) return -1;

    int pos = 1 + 8;
    const unsigned char *iv = token + pos;
    pos += IV_SIZE;
    const unsigned char *ct = token + pos;
    int ct_len = token_len - pos - HMAC_SIZE;
    const unsigned char *stored_hmac = token + pos + ct_len;

    unsigned char computed_hmac[HMAC_SIZE];
    unsigned int hl = HMAC_SIZE;
    HMAC(EVP_sha256(), hmac_key, KEY_SIGNING_HALF, token, pos + ct_len, computed_hmac, &hl);
    if (memcmp(computed_hmac, stored_hmac, HMAC_SIZE) != 0) return -1;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    *out = malloc(ct_len + 16);
    if (!*out) { EVP_CIPHER_CTX_free(ctx); return -1; }
    int olen = 0, tmp = 0;
    EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, aes_key, iv);
    EVP_DecryptUpdate(ctx, *out, &tmp, ct, ct_len);
    olen = tmp;
    EVP_DecryptFinal_ex(ctx, *out + tmp, &tmp);
    olen += tmp;
    EVP_CIPHER_CTX_free(ctx);
    (*out)[olen] = '\0';
    *out_len = olen;
    return 0;
}

static int decrypt_blob(const unsigned char *key, const char *hex, int hex_len,
                        unsigned char **out, int *out_len)
{
    int tlen = hex_len / 2;
    unsigned char *tok = malloc(tlen);
    if (!tok) return -1;
    for (int i = 0; i < tlen; i++)
        sscanf(hex + i * 2, "%2hhx", &tok[i]);
    unsigned char *aes_key  = (unsigned char *)key + KEY_SIGNING_HALF;
    unsigned char *hmac_key = (unsigned char *)key;
    int rc = do_decrypt(aes_key, hmac_key, tok, tlen, out, out_len);
    free(tok);
    return rc;
}

int main(void)
{
    const char *home = getenv("HOME");
    if (!home) { fprintf(stderr, "HOME not set\n"); return 1; }

    char salt_path[1024], pw_path[1024];
    snprintf(salt_path, sizeof(salt_path), "%s/.config/echo-ai/salt", home);
    snprintf(pw_path, sizeof(pw_path), "%s/.config/echo-ai/password", home);

    unsigned char *salt = NULL;
    int salt_len = 0;
    if (load_file(salt_path, &salt, &salt_len) != 0) {
        fprintf(stderr, "failed to load salt\n"); return 1;
    }
    char password[256];
    FILE *pwf = fopen(pw_path, "r");
    if (!pwf || !fgets(password, sizeof(password), pwf)) { free(salt); return 1; }
    fclose(pwf);
    int pwlen = strlen(password);
    while (pwlen > 0 && (password[pwlen - 1] == '\n' || password[pwlen - 1] == '\r'))
        password[--pwlen] = '\0';

    unsigned char key[KEY_SIZE];
    if (EVP_PBE_scrypt(password, pwlen, salt, salt_len,
                       SCRYPT_N, SCRYPT_R, SCRYPT_P, 512 * 1024 * 1024,
                       key, KEY_SIZE) != 1) {
        fprintf(stderr, "scrypt failed\n"); free(salt); return 1;
    }
    free(salt);

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/.config/echo-ai/echo-ai.db", home);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "sqlite3 \"%s\" "
             "\"SELECT id, title, hex(messages_encrypted) FROM agent_sessions "
             "ORDER BY created_at DESC LIMIT 5;\"",
             db_path);

    FILE *sql = popen(cmd, "r");
    if (!sql) return 1;

    char line[524288];
    while (fgets(line, sizeof(line), sql)) {
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        char *id = strtok(line, "|");
        char *title = strtok(NULL, "|");
        char *hex = strtok(NULL, "|");
        if (!id || !title || !hex) continue;

        printf("\n========================================\n");
        printf("SESSION: %s  |  %s\n", id, title);
        printf("========================================\n");

        unsigned char *plain = NULL;
        int plen = 0;
        if (decrypt_blob(key, hex, strlen(hex), &plain, &plen) == 0) {
            printf("%s\n", plain);
            free(plain);
        } else {
            printf("[DECRYPT FAILED]\n");
        }
    }
    pclose(sql);
    return 0;
}
