# Session Decryption Helper

## Build
```bash
nix develop -c gcc -o scripts/decrypt_session scripts/decrypt_session.c -lssl -lcrypto -lsqlite3
```

## Run
```bash
./scripts/decrypt_session
```

Dumps the 5 most recent sessions from `~/.config/echo-ai/echo-ai.db`, decrypting
using the Fernet-like crypto (scrypt + AES-128-CBC + HMAC-SHA256). The password
is prompted on stdin; key derivation matches `encryption_key_derive`:
scrypt(password, salt || pepper) with salt from `~/.config/echo-ai/salt` and
pepper from `~/.config/echo-ai/.pepper`.
