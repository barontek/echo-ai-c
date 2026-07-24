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
using the Fernet-like crypto (scrypt + AES-128-CBC + HMAC-SHA256) with the
password from `~/.config/echo-ai/password` and salt from `~/.config/echo-ai/salt`.
