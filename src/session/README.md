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
