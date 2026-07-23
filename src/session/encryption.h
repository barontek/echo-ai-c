#ifndef ECHO_ENCRYPTION_H
#define ECHO_ENCRYPTION_H

typedef struct {
    unsigned char key[32];
} EncryptionKey;

int encryption_key_derive(const char *password, const unsigned char *salt, int salt_len, EncryptionKey *key);
unsigned char *encryption_encrypt(const EncryptionKey *key, const unsigned char *plaintext, int plaintext_len, int *out_len);
unsigned char *encryption_decrypt(const EncryptionKey *key, const unsigned char *token, int token_len, int *out_len);
char *encryption_resolve_password(void);
int encryption_salt_create(const char *salt_path);
int encryption_salt_load(const char *salt_path, unsigned char *salt, int *salt_len);
int encryption_first_run_detect(const char *data_dir);
int encryption_create_verifier(const EncryptionKey *key, const char *path);
int encryption_check_verifier(const EncryptionKey *key, const char *path);

#endif
