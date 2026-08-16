/*
 * encryption.h - Fernet-based field-level encryption for session records:
 * scrypt key derivation, AES-128-CBC encrypt/decrypt with HMAC-SHA256
 * signing, and password/salt/verifier file handling.
 * Depends on: openssl (evp, hmac, rand), logging, string_utils.
 */

#ifndef ECHO_ENCRYPTION_H
#define ECHO_ENCRYPTION_H

typedef struct {
    unsigned char key[32];
} EncryptionKey;

/*
 * The 32-byte key is split in half: the low 16 bytes sign (HMAC-SHA256),
 * the high 16 bytes encrypt (AES-128-CBC), per the Fernet spec. Treated as
 * sensitive: session_manager scrubs copies on teardown.
 */

/**
 * encryption_key_derive - derive a 32-byte encryption key from a password
 * @password: NUL-terminated password; must be non-NULL.
 * @salt: salt bytes; must be non-NULL with @salt_len > 0.
 * @salt_len: length of @salt in bytes; max 64.
 * @pepper: pepper bytes mixed into the scrypt input, or NULL for none.
 *   When non-NULL, @pepper_len must be > 0 and <= 64.
 * @pepper_len: length of @pepper in bytes (ignored when @pepper is NULL).
 * @key: out-param; caller-provided EncryptionKey filled with the derived
 *   key on success. Not owned by this function.
 *
 * Uses scrypt (N=2^18, r=8, p=1) over salt||pepper. The pepper is a
 * per-vault secret (data_dir/.pepper, 0600): without it, a leaked copy
 * of the DB cannot be used as an offline password-checking oracle.
 * The combined salt||pepper buffer is zeroized before return.
 *
 * Return: 0 on success, -1 on invalid arguments or scrypt failure (key
 * contents unspecified). Thread-safe; no shared state.
 */
int encryption_key_derive(const char *password, const unsigned char *salt, int salt_len,
                          const unsigned char *pepper, int pepper_len,
                          EncryptionKey *key);

/**
 * encryption_encrypt - encrypt plaintext into a Fernet token
 * @key: key from encryption_key_derive(); must be non-NULL.
 * @plaintext: bytes to encrypt; must be non-NULL with @plaintext_len > 0.
 * @plaintext_len: length of @plaintext in bytes.
 * @out_len: out-param receiving the token length; must be non-NULL.
 *
 * Token layout: version byte | 8-byte big-endian timestamp | 16-byte IV |
 * AES-128-CBC ciphertext | 32-byte HMAC-SHA256. The key's low half signs,
 * the high half encrypts.
 *
 * Return: caller-owned malloc'd buffer of *out_len bytes (NOT
 * NUL-terminated), or NULL on invalid arguments, RNG failure, or
 * allocation failure. Free with free(). Thread-safe; no shared state.
 */
unsigned char *encryption_encrypt(const EncryptionKey *key, const unsigned char *plaintext, int plaintext_len, int *out_len);

/**
 * encryption_decrypt - verify and decrypt a Fernet token
 * @key: key that encrypted the token; must be non-NULL.
 * @token: token bytes from encryption_encrypt(); must be non-NULL with
 *   @token_len > 0.
 * @token_len: length of @token in bytes.
 * @out_len: out-param receiving the plaintext length; must be non-NULL.
 *
 * Recomputes and compares the HMAC before decrypting; any mismatch is a
 * failure (bad key, corrupted token).
 *
 * Return: caller-owned malloc'd plaintext buffer of *out_len bytes,
 * NUL-terminated (out_len excludes the terminator), or NULL on invalid
 * arguments, HMAC mismatch, decryption failure, or allocation failure.
 * Free with free(). Thread-safe; no shared state.
 */
unsigned char *encryption_decrypt(const EncryptionKey *key, const unsigned char *token, int token_len, int *out_len);

/**
 * encryption_salt_create - create a 16-byte random salt file
 * @salt_path: path to create; exclusive-create semantics — fails if the
 *   file already exists.
 *
 * The file is created with mode 0600 regardless of umask.
 *
 * Return: 0 on success, -1 on RNG failure, open failure, or short write
 * (a partially-written salt file is unlinked). Failures are logged.
 * Thread-safe.
 */
int encryption_salt_create(const char *salt_path);

/**
 * encryption_pepper_create - create a 32-byte random pepper file
 * @pepper_path: path to create; exclusive-create semantics — fails if
 *   the file already exists.
 *
 * The pepper is the per-vault secret mixed into scrypt by
 * encryption_key_derive(); the file is created with mode 0600.
 *
 * Return: 0 on success, -1 on RNG failure, open failure, or short write
 * (a partially-written pepper file is unlinked). Failures are logged.
 * Thread-safe.
 */
int encryption_pepper_create(const char *pepper_path);

/**
 * encryption_salt_load - read the salt file into a caller buffer
 * @salt_path: path to read.
 * @salt: caller-provided buffer of at least 64 bytes, filled on success;
 *   not owned by this function.
 * @salt_len: out-param receiving the number of bytes read; must be
 *   non-NULL.
 *
 * Rejects files larger than 64 bytes. A zero-length file loads with
 * *salt_len == 0 and is rejected downstream by key derivation.
 *
 * Return: 0 on success, -1 on open failure, oversized file, seek/tell
 * error, or short read. Failures are silent. Thread-safe.
 */
int encryption_salt_load(const char *salt_path, unsigned char *salt, int *salt_len);

/**
 * encryption_pepper_load - read the pepper file into a caller buffer
 * @pepper_path: path to read.
 * @pepper: caller-provided buffer of at least 64 bytes, filled on
 *   success; not owned by this function.
 * @pepper_len: out-param receiving the number of bytes read; must be
 *   non-NULL.
 *
 * Same contract as encryption_salt_load(); a missing pepper file is an
 * error because it renders the vault's keys underivable.
 *
 * Return: 0 on success, -1 on open failure, oversized file, seek/tell
 * error, or short read. Failures are silent. Thread-safe.
 */
int encryption_pepper_load(const char *pepper_path, unsigned char *pepper, int *pepper_len);

/**
 * encryption_first_run_detect - check whether <data_dir>/salt exists
 * @data_dir: directory containing the salt file.
 *
 * Return: 1 when no salt file exists (first run), 0 when it exists.
 * Never fails on I/O; an allocation failure while building the path is
 * treated as "not first run" (0). Thread-safe.
 */
int encryption_first_run_detect(const char *data_dir);

/**
 * encryption_create_verifier - persist a password verifier file
 * @key: key to encrypt the verifier with; must be non-NULL.
 * @path: file to create; exclusive-create semantics — fails if it
 *   already exists. Created with mode 0600.
 *
 * Writes the Fernet token of the fixed string "echo-ai-ok" so a later
 * password check has something to decrypt and compare.
 *
 * Return: 0 on success, -1 on encryption failure, open failure, or short
 * write (a partial file may remain; it is not unlinked). Failures are
 * silent. Thread-safe.
 */
int encryption_create_verifier(const EncryptionKey *key, const char *path);

/**
 * encryption_check_verifier - verify a password against the verifier file
 * @key: key derived from the password to test.
 * @path: verifier file created by encryption_create_verifier().
 *
 * Decrypts the file and compares the plaintext to "echo-ai-ok". Rejects
 * files larger than 4096 bytes.
 *
 * Return: 0 on match, -1 when the file is missing, empty, or oversized,
 * on allocation or decryption failure, or when the plaintext differs.
 * Failures are silent. Thread-safe.
 */
int encryption_check_verifier(const EncryptionKey *key, const char *path);

#ifdef ENCRYPTION_TEST
/**
 * encryption_test_validate_token - parse a Fernet token with zero keys
 * @token: raw token bytes; need not be NUL-terminated.
 * @token_len: number of bytes in @token.
 *
 * Test-only hook into the real decrypt_fernet_token layout parsing
 * (version byte, minimum length, IV/ciphertext/HMAC arithmetic) with
 * fixed zero keys, so fuzzers can hammer the bounds logic without key
 * material. A matching HMAC is effectively unforgeable by fuzzing, so
 * the decrypt branch only runs on genuinely valid tokens.
 *
 * Return: 1 when the token parses, 0 when rejected. Thread-safe; pure.
 */
int encryption_test_validate_token(const unsigned char *token, int token_len);
#endif

#endif
