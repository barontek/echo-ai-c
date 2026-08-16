# src/session — SQLite session store

What this owns: the `SessionManager` facade — session CRUD, encrypted
field storage (Fernet-style tokens via `encryption.c`), message
serialization, the branch (fork/switch) model (`session_branch.c`), the
user-memory table (`memory.c`), and the password-migration flow
(`migration.c`).

Why it exists: all durable conversation state lives behind one locked
facade so the agent and routes never touch SQLite directly.

Key contracts: `session_manager_*` requires the caller to hold the
manager lock where documented (per-function in `session_manager.h`);
loaded `Session` objects are caller-owned (free with `session_free`).

Vault key material: the derived key is `scrypt(password, salt || pepper)`
where `salt` (16 bytes) and `.pepper` (32 bytes) live in the data dir
(both created 0600 on first run). The pepper is a per-vault secret that
does not travel with a leaked DB copy, so a stolen `echo-ai.db` cannot be
used as an offline password-checking oracle. Vaults created before the
pepper change are deliberately not upgradable — first-run with a fresh
data dir is required.
