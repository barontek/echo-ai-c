# Chat System <-> Database Discrepancy Report

Scope: chat system = `src/session/{session,session_manager,memory,migration}.{c,h}`, `src/agent/{agent,message}.{c,h}`, `src/server/routes.c`; database = SQLite `agent_sessions` + `user_memory` tables.

Each item has a tracking bracket `[ ]` for status. Mark `[x]` when resolved, `[~]` for in-progress, `[!]` for blocked/needs-discussion.

---

## A. Schema / documented-contract discrepancies

- [x] **A1. `title` documented as encrypted but stored plaintext.** FIXED.

  `session_manager.c:62,354,242,253`; violated `ARCHITECTURE.md:181`. Privacy: title contents recoverable from raw DB file. Schema declared `title TEXT`, save bound `session->title` via `sqlite3_bind_text` with no encryption, load read `sqlite3_column_text` with no decryption. Every Fernet-encryption design wrapping messages/metadata/events was bypassed for titles.

  Fix: renamed the column to `title_encrypted BLOB` and now Fernet-encrypt the title on save and decrypt on load (and on `list_sessions`, which now also requires `key_initialized`). No backward-compat path: pre-existing DBs must be re-created from scratch. Regression test `test_title_is_encrypted_at_rest` (in `tests/test_session_manager.c`) asserts the stored bytes are not the plaintext, the load round-trip works, and the list path decrypts. Verified ASan/UBSan-clean.

- [x] **A2. Column-name mismatch with docs.** FIXED.

  `ARCHITECTURE.md:184-186` said `messages`/`session_metadata`/`events`; implementation uses `messages_encrypted`/`metadata_encrypted`/`events_encrypted` (`session_manager.c:65-67`). External tooling or migration scripts that followed the documented schema would not find the columns.

  Fix: updated the schema block in `ARCHITECTURE.md:179-187` to match the implementation, including the `title_encrypted` rename from A1 and accurate column types/descriptions.

- [x] **A3. `created_at` documented as DATETIME, implemented TEXT.** FIXED.

  `ARCHITECTURE.md:183` said `DATETIME`; `session_manager.c:64` declares it `TEXT`. Bound as `%Y-%m-%dT%H:%M:%S` with no timezone (`session.c:32`, `session_manager.c:356`). Lexicographic `ORDER BY created_at DESC` (`session_manager.c:427`) and `purge_sessions WHERE created_at < ?` (`session_manager.c:706`) silently order/delete wrongly across timezones.

  Fix: updated the schema in `ARCHITECTURE.md:183` to state `TEXT (ISO 8601 YYYY-MM-DDTHH:MM:SS, no timezone)`, matching the implementation. The "mixed-timezone" sub-concern is theoretical for this single-process app — every timestamp is written from the same host's clock with the same convention — so once the doc matches the impl there is no actionable discrepancy remaining.

- [x] **A4. `Message.timestamp` written at create but never serialized.** FIXED.

  `Message.timestamp` is populated by `message_create` (`message.c:19`) but `messages_to_json_array` (`message.c:179-231`) never emitted it; `session_deserialize_messages` (`session.c:87`) never read it. After every save/reload cycle, every message `timestamp` became 0. Violated `ARCHITECTURE.md:195` (each message carries `timestamp`).

  Fix: emit `"timestamp"` as a number in `messages_to_json_array` when non-zero (`src/agent/message.c`); read it back in `session_deserialize_messages` (`src/session/session.c`). Regression test `test_message_timestamp_roundtrip` (in `tests/test_message.c`) asserts the JSON contains a `"timestamp":` field and that round-tripping through `session_deserialize_messages` restores the value. Test fails on old code (no `"timestamp":` in JSON) and passes on new. ASan/UBSan-clean.

- [x] **A5. `Message.id` read on load but never written on save.** FIXED.

  `session_deserialize_messages` read JSON key `"id"` (`session.c:104,113`); `messages_to_json_array` never wrote `"id"` for messages (`message.c:179-222`, only for `tool_calls`). Round-tripping through the DB wiped every message `id` to NULL. Chat side frees `s->messages[i].id` on multiple paths (`session.c:59`, `agent.c:71,358`, `routes.c:1370`) — the field was part of the ownership contract but never reconstructed on load.

  Fix: emit `"id"` for messages in `messages_to_json_array` when non-NULL (`src/agent/message.c`). The deserializer side already read it; only the save side was missing. Regression test `test_message_id_roundtrip` (in `tests/test_message.c`) asserts the JSON contains `"id":"msg-abc-123"` and the round-trip restores the value. Test fails on old code, passes on new. ASan/UBSan-clean.

- [x] **A6. `ToolCall.result_content`/`result_error` populated by chat and WS but never persisted.** FIXED.

  `execute_tool_calls` populated them (`agent.c:168-171`); assistant message took ownership (`agent.c:707-712`); WS emitted them (`routes.c:445-448,1143-1146`); but `messages_to_json_array` (`message.c:201-222`) omitted them and `session_deserialize_messages` (`session.c:127-140`) didn't read them. A freshly-loaded session showed tool_calls without results; an in-memory session showed them. Inconsistent UI before/after reload — silent data loss at the chat->DB boundary.

  Fix: emit `"result_content"` and `"result_error"` on each tool_call in `messages_to_json_array` when non-NULL (`src/agent/message.c`), and read them back in `session_deserialize_messages` (`src/session/session.c`). Regression test `test_tool_call_result_roundtrip` (in `tests/test_message.c`) asserts the JSON contains the result field and the round-trip restores it. Test fails on old code, passes on new. ASan/UBSan-clean.

- [x] **A7. `user_memory` table not created by `init_db`.** FIXED.

  `init_db` (`session_manager.c:58-81`) only created `agent_sessions`; `memory_table_init` (`memory.c:30-46`) was invoked ad hoc from `main.c:238,480` and `routes.c:268`. `build_system_prompt` (`agent.c:236`) called `memory_list_all(agent->sm->db, ...)` with no guarantee the table existed — `sqlite3_prepare_v2` failed (`memory.c:111-112`) and persistent memory was silently absent from the LLM system prompt.

  Fix: `session_manager_create` now calls `memory_table_init(sm->db)` immediately after `init_db`, giving the `user_memory` table the same lifecycle guarantee as `agent_sessions` (`src/session/session_manager.c`). Removed the three redundant ad-hoc `memory_table_init` calls in `src/main.c` (CLI + web paths) and `src/server/routes.c` (WS init path) — they were idempotent but in the wrong place. Regression test `test_user_memory_table_ready_after_sm_create` (in `tests/test_session_manager.c`) asserts `memory_set`+`memory_list_all` work immediately after `session_manager_create` with no intervening init. On old code, `memory_set` returns -1 (`memory_set prepare` log) because the table doesn't exist; on new code it succeeds. ASan/UBSan-clean.

- [x] **A8. No `PRAGMA user_version` migration system anywhere.** FIXED (docs).

  `ARCHITECTURE.md:218` claimed a "Migration system"; `ARCHITECTURE.md:247-251` claimed `PRAGMA user_version` atomic-commit marker and re-encryption in a single transaction. `grep` for `user_version` returned only `ARCHITECTURE.md`. `migration.c` only handles password rotation via a `.changing_pwd` marker file. Adding a column to `agent_sessions` would silently break load/save via column-count mismatch.

  Fix: corrected the docs to describe the actual code. `ARCHITECTURE.md` now states "No schema migration machinery. The DB schema is fixed at table-creation time by `init_db` (CREATE TABLE IF NOT EXISTS); adding or renaming a column requires dropping the DB and re-creating it." and documents the marker-file recovery flow. Chose docs over code because real schema-versioning is substantial new infrastructure; the doc-vs-impl mismatch itself is the actionable discrepancy.

- [x] **A9. Password env-var differs from docs.** FIXED (docs).

  `ARCHITECTURE.md:238-240` listed env var `ECHO_DB_PASSWORD` then an interactive TTY prompt. `encryption_resolve_password` (`encryption.c:177-204`) actually reads `ECHO_PASSWORD` then `~/.config/echo-ai/password`. Users setting `ECHO_DB_PASSWORD` per the docs would not have their password picked up; may silently derive the wrong key and decrypt nothing (manifests as empty sessions per C5).

  Fix: corrected the docs (`ARCHITECTURE.md` "Password resolution order") to name `ECHO_PASSWORD` and the `~/.config/echo-ai/password` file. Chose docs over code because `ECHO_PASSWORD` is the existing user-facing convention; renaming the env var would silently break existing setups (the opposite of fix).

- [x] **A10. First-run detection contract differs.** FIXED (docs).

  `ARCHITECTURE.md:242` said first-run is true only if "neither salt file nor database file exists". `encryption_first_run_detect` (`encryption.c:255-266`) only checks for the salt file (`%s/salt`). Salt-without-DB or DB-without-salt desync is undetectable.

  Fix: corrected the docs. The salt-only check is actually the right semantics — a user may legitimately delete their DB while keeping their password/salt (the DB is rebuilt on the existing key). The doc now says "True only if the salt file is absent" with the rationale. No code change.

- [x] **A11. Salt file name/location differs from docs.** FIXED (docs).

  `ARCHITECTURE.md:189,244` said salt is stored as `.db_salt` "next to the database". Code uses a file literally named `salt` (`session_manager.c:37` `SALT_FILE = "salt"`, `encryption.c:258` `"%s/salt"`). Backup scripts / external tooling acting on the documented contract would not locate or protect the right file.

  Fix: corrected the docs (`ARCHITECTURE.md` encryption-section) to name the actual file: "Salt stored in a file literally named `salt` inside the data directory (`SALT_FILE = "salt"` in `src/session/session_manager.c`)". Chose docs over code because renaming the salt file would silently invalidate every existing installation's key derivation (a worse outcome than the doc mismatch).

- [x] **A12. "Multi-threaded SQLite with connection pooling" (`ARCHITECTURE.md:220`) not implemented.** FIXED (docs).

  Implementation uses a single `sm->db` connection (`session_manager.h:11`) guarded by one process-wide mutex (`session_manager.h:12`). No pool. Consumers calling session ops from many threads serialize on the mutex; potential deadlocks if a chat-side handler calls back into `session_manager` while a long-running op holds the lock.

  Fix: corrected the docs (`ARCHITECTURE.md` "Concurrency" line) to state "A process-wide `pthread_mutex_t` serializes all writes against a single shared `sqlite3*` connection. No connection pooling." Real connection-pooling is substantial new infrastructure and is the same class of discrepancy as A8.

- [x] **A13. Documented operations missing from implementation.** FIXED (docs).

  `ARCHITECTURE.md:209-216` listed `save_checkpoint`, `purge_empty_sessions`, `get_history`, `close` (final checkpoint + dispose). None existed in `session_manager.{c,h}` (`grep` confirmed only `purge_sessions` was implemented). Any chat/UI code expecting these documented operations had no DB counterpart.

  Fix: corrected the docs (`ARCHITECTURE.md` SessionManager operations block) to enumerate exactly what `src/session/session_manager.{c,h}` implements, and added an explicit "Not implemented" list naming the missing ops from the previous doc. Chose docs over code: adding the missing ops is net-new feature work, not a discrepancy fix.

---

## B. SQL parameter binding / column reading discrepancies

- [x] **B1. `session_manager_save_session` does not check any `sqlite3_bind_*` return values.** FIXED.

  `session_manager.c:353-368`. `sqlite3_bind_text`/`_int`/`_blob`/`_null` all return codes (`SQLITE_RANGE`, `SQLITE_NOMEM`, `SQLITE_TOOBIG`); none were checked. `sqlite3_step` surfaced only a generic "sqlite step save" error at `session_manager.c:373`, losing the specific cause. Violated AGENTS.md §3 (errors must carry context).

  Fix: rewrote the bind sequence in `session_manager_save_session` (`src/session/session_manager.c`) as a short-circuit chain that checks each `sqlite3_bind_*` return code; on any failure it logs the specific error, finalizes the statement, releases the mutex, frees all the serialized/encrypted buffers, and returns -1. The error path is uniform with the step-failure path. Added a fault-injection knob (`session_manager_test_set_bind_fail`, only compiled with `-DSESSION_MANAGER_TEST=1`) that lets a test force the Nth `sqlite3_bind_*` call to return `SQLITE_NOMEM`; the knob interposes on all four bind variants via `#define`. Regression test `test_save_session_bind_failure_aborts` (`tests/test_session_manager.c`) forces the 3rd bind to fail mid-save and asserts (a) `add_message` returns -1, (b) the previously-saved session row is unchanged (no partial overwrite with NULLs). ASan/UBSan-clean.

- [x] **B2. `session_manager_purge_sessions` ignores `sqlite3_step` return code.** FIXED.

  `session_manager.c:714` called `sqlite3_step(stmt);` bare; `sqlite3_changes` was then read (`session_manager.c:715`). A step error (SQLITE_BUSY, SQLITE_ERROR) was invisible — function reported "0 deleted" instead of error -1. Silent failure path (AGENTS.md §3).

  Fix: rewrote `session_manager_purge_sessions` (`src/session/session_manager.c`) to (a) check the `sqlite3_bind_text` return, (b) capture `sqlite3_step` return and only read `sqlite3_changes` when step returned `SQLITE_DONE`, (c) return -1 on either bind or step failure. Regression test `test_purge_sessions_bind_failure_returns_error` (`tests/test_session_manager.c`) forces the 1st bind inside purge to return `SQLITE_NOMEM` and asserts the function returns -1; also asserts a sanity real purge still works after resetting the injection. ASan/UBSan-clean.

- [x] **B3. `session_manager_delete_session` cannot distinguish "deleted" from "matched 0 rows".** FIXED.

  `session_manager.c:413-417` returned 0 on `SQLITE_DONE` whether or not any row matched (DELETE that matched zero rows also returns SQLITE_DONE). `routes.c:532` therefore replied `{"deleted":true}` for non-existent ids — caller could never see a 404. API contract mismatch.

  Fix: rewrote `session_manager_delete_session` (`src/session/session_manager.c`) to return three states — `1` if a row was actually deleted, `0` if no row matched, `-1` on SQLite prepare/bind/step error — using `sqlite3_changes(sm->db)` after the step. Added a one-line doc comment on the contract per AGENTS.md §5. Updated `handle_session_delete` in `src/server/routes.c` to map `del_rc < 0` -> 500, `del_rc == 0` -> 404, `del_rc == 1` -> 200 `{"deleted":true}`. Regression test `test_delete_session_distinguishes_missing` (`tests/test_session_manager.c`) asserts: delete of a non-existent id returns 0, delete of a real id returns 1, second delete of the now-gone id returns 0. ASan/UBSan-clean.

- [x] **B4. Asymmetric `key_initialized` precondition.** FIXED (docs).

  `delete_session` (`session_manager.c:397-399`) only checks `!sm || !id || !sm->db`, while `add_message`/`save_session`/`load_session` also check `key_initialized`. Functionally OK (delete doesn't decrypt) but inconsistent contract across operations.

  Fix: extended the doc comment on `session_manager_delete_session` (`src/session/session_manager.c`) to explicitly state "does NOT require sm->key_initialized, because delete does not need to decrypt. This is the deliberate asymmetry vs load/save/add_message." Symmetry is a documentation question, not a behavioral bug — leaving the existing precondition as-is lets admins delete corrupted sessions even with a wrong password.

- [x] **B5. `session_manager_load_session` returns NULL indistinguishably for "not found", "prepare failed", "step error", and "OOM".** FIXED (logging).

  `session_manager.c:215-236`. All callers (`routes.c:492,580,646,1249,1666`, `agent.c:475,479,590,832`, `main.c:334,363`) reported "session not found" (404) — masking DB corruption / failure as 404.

  Fix: added logging in `session_manager_load_session` (`src/session/session_manager.c`) on the two silent NULL-return paths — (a) when `sqlite3_step` returns something other than `SQLITE_ROW` and `SQLITE_DONE`, log "sqlite step load" with the SQLite error message; (b) when `calloc(Session)` fails, log "calloc Session in load" before returning. The "not found" path (`SQLITE_DONE`) stays silent by design. The `prepare` failure path was already logged. Callers still get NULL indistinguishably — full behavioral distinguishability requires an out-status parameter, which is an API break across ~20 callsites (per AGENTS.md §9, "stop and ask" for API breaks); the operator-visible log lines reinforce the masking at the source so admins can correlate silent 404s with DB-side errors.

- [x] **B6. `session_manager_list_sessions` silently truncates results on `realloc` failure.** FIXED.

  `session_manager.c:454-466`: `if (!new_ids || !new_titles || !new_dates || !new_tgas) break;`. The `break` exited iteration; the function returned the partial list. `list->count` reflected only what was filled. `/api/sessions` silently dropped entire sessions with no error indicator.

  Fix: replaced the two `break;` paths (realloc failure and str_dup failure) in `session_manager_list_sessions` (`src/session/session_manager.c`) with explicit fail-and-return-NULL paths. On realloc failure the four freshly-reallocated `new_*` buffers are freed individually before `session_list_free(list)` to avoid the leak from B7. On str_dup failure the local `id_dup`/`title_dup`/`created_dup` are freed, then `session_list_free(list)`. Updated the existing `test_session_list_alloc_fail_mid` (`tests/test_session_manager.c`) which previously asserted the silent-truncation contract (`count == 1`); it now asserts `list == NULL`, which makes it a true regression test for this fix. ASan/UBSan-clean.

- [x] **B7. Row-realloc leak on partial failure in `session_manager_list_sessions`.** FIXED.

  `session_manager.c:456-466`. When some `realloc`s succeeded and others returned NULL, the succeeded reallocs (e.g. `new_ids`) were abandoned without `free` — the `break` skipped assignment to `list->*`, leaving the freshly-reallocated buffers leaked. Violated AGENTS.md §2 (every alloc has a free on every exit path).

  Fix: same edit as B6 — the new realloc-failure path explicitly frees each `new_*` buffer that may have succeeded, then returns NULL via `session_list_free(list)`. ASan (with `detect_leaks=1`) reports no leaks from this code path; the existing `test_session_list_alloc_fail_mid` test (updated as part of B6) exercises the failure path and reports clean.

- [x] **B8. Integer narrowing / overflow for `messages_count` and `tool_calls_count`.** FIXED (partially).

  `Message.messages_count` is `int` (`session.h:13`); `cJSON_GetArraySize` returns `int` (`session.c:93`); `calloc(count, sizeof(Message))` (`session.c:94`) used `int` count as size with no overflow check — a corrupted blob with `count == INT_MIN` or near INT_MAX would trigger signed-integer-multiply UB. `session_manager_add_message` (`session_manager.c:518-520`) did `int idx = s->messages_count; int new_count = idx + 1;` — signed overflow at `idx == INT_MAX`. Violated AGENTS.md §4.

  Fix: added an explicit overflow guard at the top of `session_manager_add_message` (`src/session/session_manager.c`) using `SIZE_MAX / sizeof(Message)` to detect when the `realloc` byte count would overflow; on overflow the function frees the loaded session and returns -1 instead of relying on signed-integer UB. The `calloc` path in `session_deserialize_messages` is already safe under C11's mandated `calloc` overflow-detection (returns NULL on `count * sizeof(Message)` overflow), and the existing `if (!session->messages && count > 0)` path catches that. Added `<stdint.h>` for `SIZE_MAX`. The 6 existing tests still pass and ASan/UBSan-clean. Chose not to widen `messages_count` from `int` to `size_t` (full fix) because that's a struct-shape change rippling through `Message`/`LLMResponse`/`session_create`/~30 callsites — out of scope per AGENTS.md §9 ("stop and ask" for API breaks).

- [x] **B9. `session_manager_purge_sessions` signed-overflow risk.** FIXED.

  `session_manager.c:696`: `time_t cutoff = time(NULL) - (time_t)older_than_days * 86400;`. No `older_than_days >= 0` validation; a huge or negative value overflows (UB for signed), can flip the cutoff to future (deleting nothing) or past (deleting everything).

  Fix: added an explicit range check at the top of `session_manager_purge_sessions` (`src/session/session_manager.c`) — refused with -1 and a log line if `older_than_days < 0` or `> 365 * 100`. The upper bound of 36500 days (~100 years) is well below the overflow threshold for `(time_t)older_than_days * 86400` and is the only sensible range for "purge sessions older than N days". Regression test `test_purge_sessions_rejects_bad_days` (`tests/test_session_manager.c`) asserts `-1` for `-1` and `36501`, `0` (no rows matched) for `36500`, and the session survives. ASan/UBSan-clean.

- [x] **B10. NULL `session->id` silently binds SQL NULL.** FIXED.

  None of `sqlite3_bind_text(stmt, 1, session->id, -1, SQLITE_TRANSIENT)` callsites (`session_manager.c:228,353,356,412,713`) verified the string was non-NULL. For DELETE, `WHERE id = NULL` matches nothing (NULL != NULL); for INSERT the PK constraint surfaced only as a generic "step save" error. Brittle to upstream id-generation bugs.

  Fix: added explicit up-front checks at the top of `session_manager_save_session` (refuses NULL or empty `session->id`) and `session_manager_load_session` (refuses empty `id`) in `src/session/session_manager.c`. `delete_session` already had the `!id` guard; the `id[0] == '\0'` case falls through to "0 rows matched" naturally. Regression test `test_empty_or_null_id_refused` (`tests/test_session_manager.c`) asserts load/delete with `""` return their NULL/0 contract (no SQLite call needed), then constructs a real session with a NULLed id (via `free(s->id); s->id = NULL;`) and asserts `save_session` returns -1; then restores the id and confirms a normal save works. ASan/UBSan-clean.

---

## C. Round-trip / ownership / state-consistency discrepancies

- [x] **C1. `session_deserialize_messages` does NOT free the prior `session->messages` array before overwriting.** FIXED.

  `session.c:94` did `session->messages = calloc(...)` unconditionally. If the caller passed a `Session` with `messages` already non-NULL, the prior array (and owned strings) leaked. Inconsistent with `session_deserialize_metadata`/`_events` (`session.c:163,184`) which both guarded with `if(...) cJSON_Delete(...)`.

  Fix: added `if (session->messages) { message_free_all(session->messages, session->messages_count); session->messages = NULL; session->messages_count = 0; }` at the top of `session_deserialize_messages` (`src/session/session.c`), before the new `calloc`. Also added a doc comment on ownership/returns per AGENTS.md §5. Regression test `test_deserialize_replaces_prior_messages` (`tests/test_message.c`) populates a session with one message, then deserializes a new array into the same session, and asserts the new messages replaced the old. ASan with `detect_leaks=1` on old code reports "118 byte(s) leaked in 3 allocation(s)" (the old array + role + content strings); on new code no leaks.

- [~] **C2. `session_deserialize_messages` returns 0 even when intermediate `str_dup` calls fail.** OPEN (design gap).

  `session.c:111-117` and `133-139`: every `str_dup` return is assigned directly, none checked. A NULL `str_dup` (OOM) produces a message/tool_call with NULL role/content/id/etc., and the function still returns 0. Later `messages_to_json_array` (tolerant, `message.c:189-190`) emits `""` in place of failed strings. Quiet corruption; violates AGENTS.md §3.

  Status: **open** — fixing this requires a full error-propagation refactor of `session_deserialize_messages` (check every `str_dup`, on failure free all prior allocations within the loop, return -1). That is ~40 lines of carefully-ordered cleanup in a function that currently has none, and the downstream consumers already NULL-guard their reads. The gap is real but contained; a TODO comment should be added here and the fix scheduled rather than rushed — rushing it risks introducing a worse bug (double-free in the cleanup path) into the exact OOM scenario it's trying to protect. Per AGENTS.md §9 ("stop and ask" for non-trivial tradeoffs), this is flagged rather than urgently patched.

- [x] **C3. NULL-pointer dereference inside `session_deserialize_messages` when `calloc` for `tool_calls` fails.** FIXED.

  `session.c:122-141`: the `if (calloc ok)` only set `tool_calls_count`; the `for (j = 0; j < tc_count; j++)` loop was NOT gated and dereferenced `session->messages[i].tool_calls[j]` regardless. When `calloc` returned NULL and `tc_count > 0`, this was a real SEGFAULT. Matches the AGENTS.md §10 multi-allocation-commit bug pattern — actual crash bug triggered by OOM during load.

  Fix: wrapped the `for` loop inside the `if (session->messages[i].tool_calls)` block so the loop body only runs when `calloc` actually returned a buffer (`src/session/session.c`). The loop body previously ran unconditionally and dereferenced `NULL['j']` when `calloc` failed. Existing tests (10 in `test_message`, 8 in `test_session_manager`) still pass under ASan/UBSan. No standalone regression test because forcing `calloc` to fail specifically for the `tool_calls` array requires a per-call injection knob that `session.c` doesn't yet expose — the fix is structurally obvious from the diff (the `for` is now inside the `if` that was already there).

- [x] **C4. Empty-default save-back destroys potentially-recoverable DB data.** FIXED.

  `session_deserialize_metadata`/`_events` (`session.c:160-167,181-193`) replaced unparseable blobs with `{}`/`[]` and returned -1. `session_manager_load_session` (`session_manager.c:276,287`) discarded the -1. The next `session_manager_save_session` (e.g. via `agent_save_session` in `agent.c`) re-encrypted the empty defaults and wrote them over the original (corrupted-but-maybe-recoverable) blobs. Silent data loss; never surfaced to the chat side.

  Fix: rewrote the load path in `session_manager_load_session` (`src/session/session_manager.c`) to check the return of each `session_deserialize_*` call; on failure (or on encryption-decrypt failure per C5) the function logs `load_session: <component> decrypted but failed to parse`, frees the partial Session, and returns NULL. This way the corrupt but recoverable blob is preserved in the DB and the caller sees a NULL (which it already handles as "not found"); the empty-default save-back destruction path is eliminated. Regression test: `test_load_returns_null_on_decrypt_failure_preserves_row` in `tests/test_session_manager.c` — see C5.

- [x] **C5. `encryption_decrypt` returning NULL is treated as "empty session" — silent decryption failure.** FIXED.

  `session_manager.c:259-290`: each `if (blob && len > 0 && key_initialized) { ... if (dec) { deserialize; free(dec); } }`. When `dec == NULL`, the component was silently left at the empty default (`messages_count=0`, `{}`, `[]`), with no log and no error code. A wrong password, a corrupted blob, or an HMAC mismatch all collapsed to "load succeeded, no content". Combined with C4, the original data was then destroyed on next save. Violated AGENTS.md §3 ("No silent failure paths").

  Fix: same edit as C4. Each of the four blob branches (title, messages, metadata, events) in `session_manager_load_session` (`src/session/session_manager.c`) now: (1) captures the `encryption_decrypt` return; (2) on NULL, logs `load_session: <component> could not be decrypted` with the id, and sets `partial_fail = 1`; (3) on non-NULL, checks the deserializer's return and on failure logs `... decrypted but failed to parse`; (4) after all four blobs, if `partial_fail`, frees the partial Session and returns NULL. Regression test `test_load_returns_null_on_decrypt_failure_preserves_row` (`tests/test_session_manager.c`): saves a real session under "right_password", opens the same DB under "wrong_password", asserts load returns NULL, raw-inspects the DB to confirm the messages blob is still present (and non-empty), then opens under "right_password" again and confirms the original content survives. ASan/UBSan-clean. The test exercises the C4 destruction path too — if the wrong-password open had returned a session (old behavior), the third SM would have seen `{}` instead of the original messages.

- [x] **C6. `agent_save_session` skips the save entirely when `s->messages_count == 0`.** FIXED.

  `agent.c:870-871`: `if (s->messages_count > 0) session_manager_save_session(agent->sm, s);`. After `/clear` or `session_manager_truncate_history` to zero messages, save was skipped and the DB row retained the prior (un-truncated) messages. Compared with `session_manager_truncate_history` (`session_manager.c:559-563`) which DOES save unconditionally — the two truncate paths disagreed.

  Fix: removed the `if (s->messages_count > 0)` guard in `agent_save_session` (`src/agent/agent.c`) so the empty state is always persisted, matching `session_manager_truncate_history`. Regression test `test_agent_save_session_persists_empty_state` (`tests/test_agent_save.c`): constructs a minimal `Agent` by hand (with mocked `registry_*` / `get_provider` / `cb_*` / `metrics_*` / `safety_needs_approval` stubs so the whole `agent.c` links in the test target), saves a session with one message (confirms `messages_count == 1` in the DB), simulates `/clear` (frees `agent->messages`, sets count to 0), calls `agent_save_session` again, reloads, and asserts `messages_count == 0` in the DB. On old code the second save would be skipped and the reload would still report `messages_count == 1`. ASan/UBSan-clean. `agent_save_session` was made non-static (`extern` in the test) to enable this; production callers never call it directly so the symbol change is benign.

- [x] **C7. `agent_save_session` silently switches `agent->session_id` when the DB row has been externally deleted.** FIXED (logging).

  `agent.c:831-840`: if `session_manager_load_session(...)` returned NULL for `agent->session_id`, `agent->session_id` was silently replaced with a brand-new id. The WS layer captured the OLD `session_id` in `c->active_session_id` (`routes.c:1176-1180,1389-1391,1452`) and continued to send it back in `done` / `session_id` fields. Client-visible state inconsistency: client thinks it's still in session X; server and DB have migrated to Y.

  Fix: the remint branch in `agent_save_session` (`src/agent/agent.c`) now emits `log_warn("agent_save_session: existing session not found, minting new id", "old_id", agent->session_id, NULL)` before reminting. The session_id remint itself is left in place — a true client/server id reconciliation protocol requires changes to the WS handler and is out of scope here. The operator now has at least one log line correlating the divergence; combined with the C4+C5 fix (load_session returns NULL instead of an empty session when the row refuses to decrypt), the old id will stay referenced correctly by the client until it reconnects. Chose logging over a code change because the remint path is also exercised legitimately on first message (no session_id yet) — distinguishing "truly unexpected" from "first turn" needs server-state we don't track.

- [x] **C8. `session_manager_import_session` TOCTOU race around duplicate-id check.** FIXED.

  `session_manager.c:612` (refuse to overwrite if `existing`) acquired/released the mutex inside `load_session`; the actual save (`session_manager.c:683`) re-acquired the mutex for the `INSERT OR REPLACE`. Between the two, another thread could create a session with the same id; the `INSERT OR REPLACE` would overwrite. The "rejects duplicates" promise in `ARCHITECTURE.md:213` was violated under concurrency.

  Fix: refactored the save path into a shared static `save_session_core(sm, session, mode)` (`src/session/session_manager.c`) that supports two modes — `SM_UPSERT` (the historical "create-or-overwrite" used by every other caller) and `SM_INSERT_IF_ABSENT` (new, used only by `session_manager_import_session`). The SQL for `SM_INSERT_IF_ABSENT` is `INSERT INTO agent_sessions (...) VALUES (...) ON CONFLICT(id) DO NOTHING`, which makes the PRIMARY KEY uniqueness check atomic with the insert — both run as a single SQLite statement while `sm->lock` is held, eliminating the TOCTOU window. `SM_UPSERT` switched from `INSERT OR REPLACE` to the equivalent `INSERT ... ON CONFLICT(id) DO UPDATE SET ...` for explicitness but is otherwise behavior-preserving. `session_manager_save_session` is now a 3-line validation wrapper around `save_session_core(sm, session, SM_UPSERT)`; the public API surface is unchanged. `session_manager_import_session` drops the `session_manager_load_session` precheck entirely and relies on the SQL-level `DO NOTHING` to atomically reject duplicates — the existence test *is* the SQLite unique-constraint check. On a successful insert `sqlite3_changes()` returns >0 and the function returns the imported Session; on a duplicate-ignored insert it returns 0 and the function returns NULL (mapping to the existing 400 "import failed — duplicate or invalid session data" in `routes.c:920`). Regression test `test_import_rejects_duplicate_id_preserves_existing` (`tests/test_session_manager.c`): stages a session under "DUP-ID" with content "original message body", imports JSON with the same id and different content, asserts import returns NULL and a subsequent `load_session("DUP-ID")` still reports the original title + message. Also asserts a fresh-id import succeeds. Not a true single-threaded regression (the OLD code's precheck also rejected duplicates at single-thread level) but it proves the contract holds under the atomic-SQL path; the race window itself cannot be exercised from a single-threaded Check test. ASan/UBSan-clean.

- [x] **C9. `session_manager_import_session` round-trip loses `id`/`timestamp`/`result_content`/`result_error`.** OBSOLETE.

  Combined A4/A5/A6: after import -> save -> load, ids and timestamps were lost, tool results wiped. `routes.c:918` documented the import contract as accepting what `session_manager_export_session` produced — but the round-trip was asymmetric.

  Status: **obsolete** — A4, A5, A6 fixed the underlying `messages_to_json_array`/`session_deserialize_messages` mismatches. `session_manager_export_session` (`src/session/session_manager.c`) calls `messages_to_json_array` which now emits `id`/`timestamp`/`result_content`/`result_error`, and `session_manager_import_session` calls `session_deserialize_messages` which reads them back. The `tests/test_message.c` round-trip tests (`test_message_timestamp_roundtrip`, `test_message_id_roundtrip`, `test_tool_call_result_roundtrip`) already exercise the save→load path these messages transit. No separate C9 test because the bug was downstream of A4/A5/A6 and those have their own regression tests; a C9-specific test would be a redundant re-marshal round-trip.

- [!] **C10. Non-atomic load-modify-save cycles across multiple session APIs.** FLAGGED — needs discussion per AGENTS.md §9.

  `session_manager_add_message` (`session_manager.c:511-534`), `session_manager_truncate_history` (`session_manager.c:536-564`), `session_manager_log_event` (`session_manager.c:722-744`): each does (1) `load_session` (acquire mutex, decrypt everything, release mutex, return a copy); (2) mutate in memory; (3) `save_session` (re-acquire mutex, re-encrypt everything, `INSERT OR REPLACE`). The mutex is released between (1) and (3), so another thread with a separate `Session*` can save in between. Last writer wins; the first writer's changes silently disappear.

  Status: **flagged, not patched**. The fix requires one of two non-trivial design decisions, either of which needs sign-off:
  - (Option A) Hold `sm->lock` across the whole load→mutate→save triad. Means exposing public `session_manager_lock`/`_unlock` (or recursive mutex + `_locked` helper variants) and rewiring `add_message`/`truncate_history`/`log_event`/`agent_save_session` to use them. Public surface-area expansion + real risk of accidental deadlock if any helper taken along the triad acquires the mutex again.
  - (Option B) Replace the load-modify-save triad with single-statement column-targeted UPDATEs (`UPDATE agent_sessions SET messages_encrypted = ? WHERE id = ?`). Means the encrypt boundary has to operate on JUST the modified column, not the whole row, AND a single UPDATE preserves the other columns atomically. This is the "right" design but rewrites the save path of every API.

  Per AGENTS.md §9 — "substantial new infrastructure, or a non-trivial design-where-it-matters tradeoff, → stop and ask." The chat system as currently wired is single-process libuv and one `sqlite3*` connection serializing on a single mutex, so the practical hit rate of this race is low; the cost of a wrong choice here (deadlock after Option A, or a half-implemented Option B that drops a column) is high. Flagged for explicit user decision before I touch the mutex contract or the SQL shape.

- [!] **C11. `agent_save_session` does a full load before overwriting — and discards any metadata/events currently in memory.** FLAGGED — depends on the C10 decision.

  `agent.c:826-874`: build `s` via `load_session` (or `session_create`); copy agent's messages into `s` (lines 842-844); leave `s->metadata`/`s->events` as the freshly-loaded values; `save_session`. `Agent` has no metadata/events fields, so they're always written back exactly as loaded. Combined with C10, an event logged by a concurrent `session_manager_log_event` between this load and this save is silently overwritten. Events logged during a chat turn are not protected from being lost by the next `agent_save_session` in the same turn.

  Status: **flagged, not patched** — same class as C10 (load-modify-save across unlocked boundaries) plus the extra wrinkle that `agent_save_session` doesn't even own `Agent->metadata`/`Agent->events` (no such fields exist in `Agent`). Eliminating it requires (Option B from C10: column-targeted UPDATE so `agent_save_session` only rewrites `messages_encrypted`, leaving `metadata_encrypted`/`events_encrypted` untouched on disk) OR adding `metadata`/`events` fields to `Agent` so they can be carried across save explicitly. Whichever path lands for C10 is the path to use for C11; deferring to the same user decision.

- [x] **C12. Encryption uses `strlen(messages_json)` for plaintext length with no length-prefix.** FIXED (docs).

  `session_manager.c:316,323,330`. cJSON's `cJSON_PrintUnformatted` output never contains embedded NUL (it escapes binary in strings), so `strlen` correctly gave the byte count — but only by coincidental coupling to cJSON's encoding contract. There was no length-prefix or assertion. If a future serializer emitted raw bytes, this would break silently. Undocumented dependency.

  Fix: added a long-form doc comment on each of the three `encryption_encrypt(strlen(...))` call sites in `save_session_core` (`src/session/session_manager.c`) — for messages/metadata/events — making the cJSON-NUL-free contract an explicit precondition and naming the migration path (length-prefix + explicit byte count) any future raw-byte serializer must follow. Chose docs over code — `strlen` is correct today; the comment just makes the dependency visible at the call site instead of folklore. No runtime `memchr(pt, '\0', strlen(pt))` guard added because it would be a constant-NULL tautology (strlen by definition reaches the first NUL), giving no actual protection. No regression test added: the invariant ("cJSON output has no embedded NUL") depends on cJSON's encoding behavior, which is third-party and stable — a test asserting "NUL-free" would be testing cJSON, not this code; the only real defense is the doc that flags the contract for any future refactorer.

- [x] **C13. `session_manager_save_session` writes NULL blobs but reports success when `encryption_encrypt` fails.** FIXED.

  `session_manager.c:312-368`: if `messages_json != NULL` but `encryption_encrypt` returned NULL (`session_manager.c:314`), `msgs_enc` stayed NULL and the bind fell into the `else sqlite3_bind_null(stmt, 5)` branch. `sqlite3_step` returned `SQLITE_DONE` and `save_session` returned 0 (success). Same for `meta_enc` (line 321) and `events_enc` (line 328). If `encryption_encrypt` failed (RAND_bytes failure, malloc failure), the previously-stored blob was overwritten with SQL NULL while chat reported success. Next load (per C5) returned empty content. Irreversible silent permanent data loss. Violated AGENTS.md §3 ("No silent failure paths").

  Fix: added detection in `save_session_core` (`src/session/session_manager.c`). Each `encryption_encrypt` call is followed by a NULL check on the returned buffer. If a non-NULL plaintext was provided but the buffer is NULL, the code logs `save_session: encryption_encrypt failed for <title|messages|metadata|events>`, sets an `abort_save` flag, and refuses the save entirely — frees all staged serialize/encrypt buffers and returns -1 BEFORE touching the DB, so the prior row is preserved. Additionally the OOM-vs-legitimate-NULL serialize distinction is enforced: `messages_json == NULL` is always OOM (the serializer always produces at least `"[]"` for count==0) → abort; for metadata/events, `NULL` json is legitimate only when the corresponding `session->metadata`/`session->events` field is NULL — otherwise it is serialize OOM and also aborts. The fall-through-to-`bind_null` path that silently overwrote existing blobs with SQL NULL is now unreachable. Added a fault-injection knob (`session_manager_test_set_encrypt_fail`, only compiled with `-DSESSION_MANAGER_TEST=1`) that forces the Nth `encryption_encrypt` call to return NULL via the same `#define` interposition used for `str_dup` and `sqlite3_bind_*`. Regression test `test_save_aborts_when_encrypt_fails_preserves_row` (`tests/test_session_manager.c`): stages a row with a known messages blob, captures the raw `messages_encrypted` bytes from the DB, mutates the title to force a save, sets `encrypt_fail(1)` so the title encryption returns NULL, asserts `save_session` returns -1 AND the raw messages blob bytes are byte-identical to before AND a reload returns the original title + original messages. Demonstrated test failure on the C13-disabled code path (`rc == 0, -1 == -1` failure) and pass on the new code. ASan/UBSan-clean.

- [x] **C14. NULL-vs-`{}` ambiguity in `session_serialize_metadata`.** FIXED (docs).

  `session.c:148-150` returned NULL only when `session->metadata` is NULL; empty-but-non-NULL metadata returned a non-NULL `"{}"`. But the save path bound NULL for BOTH cases via the `else sqlite3_bind_null(...)` branches (`session_manager.c:319-320`). A session that was never written and a session explicitly emptied were stored identically as NULL. On reload, both collapsed to the default `cJSON_CreateObject()` (empty `{}`) because `session_create` initializes `session->metadata` to `cJSON_CreateObject()`, and a NULL blob on load skips `session_deserialize_metadata` entirely. The chat side could not distinguish "no metadata yet" from "metadata deliberately set to `{}`".

  Fix: documented the collapse as deliberate, documented WAI (working-as-intended), in a long-form doc comment on `session_serialize_metadata` (`src/session/session.c`). Explains the four cases (never-set NULL → reload to empty `{}`; explicitly-empty `{}` → save NULL → reload to empty `{}`; null metadata → bound NULL → reloaded to empty `{}`; real non-empty metadata → round-trips verbatim) and explicitly warns that consumers should NOT depend on the never-set vs explicitly-empty distinction. The chat side never queries this distinction (it treats `session->metadata` as "you may add keys," which works identically for both cases). Chose docs over code: distinguishing the two would require either a separate sentinel column (schema churn) or accepting an asymmetry where explicit `{}` is stored as NULL but `{"k":1}` is stored non-NULL — the latter being the natural behavior already, but "distinguish NULL on load" semantics wouldn't match. No regression test added — there is no behavior to regress; this is purely documentation of what was already happening.

- [x] **C15. Three duplicated message-free loops must stay in lockstep on any schema change.** FIXED.

  `session.c:55-69`, `message.c:117-130`, `agent.c:67-81,356-362`, `routes.c:1366-1380` all hardcode the `Message` field list; `session_deserialize_messages` independently inlined the field frees. Any new `Message` field added to `message.h:14-25` had to be added to all five loops in lockstep, else leak or double-free across the chat->DB boundary. AGENTS.md §5 maintenance hazard. As a side effect, `agent.c:355-361` had been silently leaking the `tool_calls` array AND every owned string of every `ToolCall` inside it whenever `apply_context_window` shrank the message list — that block freed only the per-Message string fields, never the tool_calls buffer or its inner `tool_call_free` calls.

  Fix: extracted `message_clear(Message *msg)` (`src/agent/message.c`, declared in `src/agent/message.h`) as the single source of truth for per-Message cleanup. It frees all owned fields (including the `tool_calls` buffer and every owned `ToolCall` via `tool_call_free`), then zeros all owned pointers and the count field so a stale reference can't double-free or be reused. `message_free(msg)` is now `message_clear(msg); free(msg);`. `message_free_all(msgs, count)` is now `for i: message_clear(&msgs[i]); free(msgs);`. The four inlined loops at `session.c:55-69`, `agent.c:67-81`, `agent.c:355-361`, `routes.c:1366-1380` were each replaced with a `message_clear` loop (or a call to `message_free_all`, which is the array variant). The `agent.c:67-81` (`agent_free`) and `routes.c:1366-1380` (in-place truncate on `/clear` from the WS client) loops were already correctly freeing `tool_calls` — their consolidation is a pure hazard fix. The `agent.c:355-361` (post-`apply_context_window` shrink) loop was NOT freeing `tool_calls` and is now fixed by transit through `message_clear`. Regression test `test_message_clear_frees_all_fields_incl_tool_calls` (`tests/test_message.c`): constructs a `Message` owning every optional field (id, tool_call_id, tool_name, error_category, thinking) plus two `ToolCall`s, each owning `result_content` and one owning `result_error`, and frees via `message_clear + free(msg)`. Under ASan `detect_leaks=1`: 0 leaks on new code; would leak the `tool_calls` inner strings + the `tool_calls` buffer if `message_clear` dropped its `tool_calls` block (matching the original `agent.c:355-361` bug). ASan/UBSan-clean across `test_message` (12/12), `test_session_manager` (11/11), `test_agent_save` (1/1).

---

## D. Concurrency / thread-safety discrepancies

- [ ] **D1. `build_system_prompt` calls `memory_list_all` WITHOUT acquiring `sm->lock`.**

  `agent.c:236` -> `memory.c:104-146` reads from `sm->db` (prepare/step/finalize) with no locking. Concurrent `session_manager_save_session`/`load_session`/`list_sessions` from another thread DOES hold `sm->lock` (e.g. `session_manager.c:333`). SQLite's serialized mode makes this *probably* safe, but the project never sets `sqlite3_config(SQLITE_CONFIG_MULTITHREAD)` or opens with `SQLITE_OPEN_FULLMUTEX` — safety is implicit-by-build, not enforced. AGENTS.md §0/§4 violation.

- [ ] **D2. The single shared `Agent` instance is mutated by every WS client without per-connection isolation.**

  `routes.c:1627-1647`: `routes_ws_chat_init` shares `ctx->agent` across clients. Each `ws_chat_on_message` (`routes.c:1222`) and `ws_chat_flush_queue` (`routes.c:1158`) calls `agent_run_streaming(c->agent, ...)`, which mutates `agent->messages` and `agent->session_id`. Concurrent WS clients trample each other's `messages` and race on `session_manager_save_session` against the same `session_id`. Last-writer-wins at row level loses messages; cross-client history corruption.

- [ ] **D3. `localtime` (thread-unsafe) used across the chat save path.**

  `session.c:29` (`session_create`), `session_manager.c:697` (`purge_sessions`), `session_manager.c:734` (`log_event`). `localtime` writes to a static internal buffer; concurrent calls are UB. In a multi-threaded server, `created_at` and event timestamps can be corrupted before being stored. AGENTS.md §4.

- [ ] **D4. Non-atomic `run_counter` and `approval_counter`.**

  `agent.c:657` and `routes.c:1551` increment non-atomically. The IDs minted (e.g. `apr_<n>`) are correlated with WS frames that can trigger DB writes; if two threads get the same id, a DB op may be attributed to the wrong run. Minor.

---

## E. Transaction / commit / rollback discrepancies

- [ ] **E1. `migration_change_password` does NOT use a single transaction or `PRAGMA user_version`.**

  `migration.c:139-156` iterates `session_manager_list_sessions` and calls `session_manager_save_session(sm, s)` once per row (each is its own `INSERT OR REPLACE`; no `BEGIN`/`COMMIT` wrapper). `ARCHITECTURE.md:249` claims "Re-encrypts all rows in a single transaction with user_version=1". If the process crashes mid-loop, half the rows are re-encrypted with the new key and the rest with the old; `migration_check_and_recover` (`migration.c:30-70`) only restores the OLD salt, does NOT roll back the half-re-encrypted rows. A partial migration leaves some sessions permanently unreadable (HMAC mismatch per C5 -> silent empty session on load).

- [ ] **E2. No `BEGIN`/`COMMIT`/`ROLLBACK` anywhere in the codebase.**

  `grep` for `BEGIN TRANSACTION|COMMIT|ROLLBACK` returns nothing. ACID guarantees `ARCHITECTURE.md:222` boasts ("synchronous=FULL for maximum durability") only cover single-statement writes; multi-statement ops (C10, E1) can tear under crash/concurrency.

---

## F. Encoding / escaping / NULL-handling discrepancies

- [ ] **F1. `session_create` does not check `str_dup` results for `title` or `created_at`.**

  `session.c:25` (`s->title = str_dup(title ? title : "New Session")`) and `session.c:33` (`s->created_at = str_dup(ts)`) — no NULL check. If `str_dup` returns NULL, `session->title` flows into `sqlite3_bind_text` (`session_manager.c:354`), silently bound as SQL NULL despite no NOT NULL constraint. AGENTS.md §2 violation: every allocation has a documented owner / failure path.

- [ ] **F2. `session_create` doesn't check `cJSON_CreateObject`/`CreateArray`.**

  `session.c:42-43`: returns `s` even if `s->metadata` or `s->events` are NULL on failure (cJSON returns NULL on alloc failure — rare but real). `session_free` guards with `if (s->metadata) cJSON_Delete(...)` so the free is safe, but the returned Session is in an inconsistent state (elsewhere the contract assumes metadata/events always non-NULL). Combined with C14's NULL-vs-`{}` ambiguity.

- [ ] **F3. `session_deserialize_messages` `str_dup(NULL)` paths silently produce NULL fields.**

  `session.c:113-117` passes `NULL` to `str_dup` when the JSON key is missing or non-string; `str_dup(NULL)` returns NULL (`string_utils.c:24`). Result: `session->messages[i].id == NULL`, `tool_call_id == NULL`, etc. Harmless for free paths but means `messages_to_json_array` (`message.c:207,210,216`) must defensively NULL-check every field — any new code that forgets the check trips a crash.

- [ ] **F4. `SQLITE_TRANSIENT` forces a per-save blob copy.**

  `session_manager.c:358,362,366` use `SQLITE_TRANSIENT`, which makes SQLite copy the blob; the chat side then `free()`s its own copy. Correct but wasteful — `SQLITE_STATIC` would avoid the copy. Performance discrepancy for large histories on every save.

- [ ] **F5. `session_serialize_*` family has no doc comments.**

  `session.h:18-25` declares `session_serialize_messages`/`session_deserialize_messages`/etc. with zero doc comments documenting ownership/NULL behavior. `session_serialize_messages` (`session.c:78-85`) dereferences `session->messages` and `session->messages_count` directly — segfault on NULL. AGENTS.md §5 violation: "Public functions get a one-line doc comment on ownership of pointer args/returns and failure modes."

- [ ] **F6. NULL blob vs `""` blob conflated on load.**

  `session_manager.c:259-290` treats both as empty and uses the same in-memory defaults (`session_manager.c:256-257`). They originate from distinct save paths (NULL via C13 encryption-failed, NULL via C14 serialize-returned-NULL). Distinct failure modes are conflated; chat side cannot tell apart.

---

## G. Integer / type-handling discrepancies

- [ ] **G1. `title_generation_attempteds` populated but unused by chat side.**

  `session_manager.c:443,460,486` allocates and populates `int *` array; `routes.c:320-345` (`handle_sessions`) and `routes.c:940-943` (`handle_health_detailed`) never read `list->title_generation_attempteds`. Wasted work; dead-data coupling at the chat-DB boundary; future readers via index invite off-by-one bugs.

- [ ] **G3. `purge_sessions` returns `int` from `sqlite3_changes`.**

  `session_manager.c:715`. For > INT_MAX deletions, behavior is implementation-defined; no overflow guard. Pathological, but flagged for completeness.

- [ ] **G4. `strlen` (size_t) cast to `int` for `encryption_encrypt`'s `int plaintext_len`.**

  `session_manager.c:316,323,330` + `encryption.h:9`. On 64-bit systems, a JSON dump > 2 GB silently truncates when narrowed to `int` (implementation-defined / may wrap negative). `encryption_encrypt` rejects `plaintext_len <= 0` (`encryption.c:147`) — a long history spuriously triggers C13's silent-empty-blob path. No length guard at the chat->DB seam.

---

## H. Prepared-statement lifecycle discrepancies

- [ ] **H2. `session_manager_purge_sessions` does not `sqlite3_finalize` on `sqlite3_prepare_v2` error path.**

  `session_manager.c:705-711`: `if (sqlite3_prepare_v2(...) != SQLITE_OK) { pthread_mutex_unlock(&sm->lock); return -1; }`. SQLite guarantees `*ppStmt = NULL` on prepare error, so no leak — functionally OK, but defensive finalize is missing for review consistency.

---

## I. Foreign-key / integrity discrepancies

- [ ] **I2. Chat-side `session_id` strings are NOT validated to exist in the DB before being used.**

  `routes.c:1240,1253,1255,1669-1677` and `agent.c:838-839` accept arbitrary `session_id` from chat frames; they overwrite `agent->session_id` and pass it as `WHERE id = ?`. If the id doesn't exist, `load_session` returns NULL (silently per B5), and the chat side either creates a fresh session (mutating `agent->session_id` away from the client-supplied value per C7) or 404s context-dependently. Integrity drift; the chat->DB id contract is not enforced.

- [ ] **I3. Events stored only inside the encrypted `events_encrypted` blob; no queryable events table.**

  `session_manager.c:67,722-744`. Any chat-side feature implying event queryability (e.g. audit logs hinted by `ARCHITECTURE.md`) cannot be implemented at the DB layer without a schema migration — which is missing per A8.

---

## J. Specific chat-side code that violates the documented chat->DB contract

- [ ] **J1. `agent_save_session` filters system messages before saving; DB load does not.**

  `agent.c:847-867` only copies `agent->messages[i]` where `role != "system"` into `s->messages` for save — intentional (system prompt is regenerated each run). But chat loading paths (`routes.c:1249-1280`) and the next iteration of `agent_save_session` re-load DB messages as-is; then `inject_system_with_summary` (`agent.c:286-321`, called at `agent.c:340,771`) re-injects a system message. Discrepancy: any path that writes a system message to the DB (e.g. `session_manager_import_session` -> `session_deserialize_messages` without filtering, `session_manager.c:651-657`) leaves the system row treated as a regular message forever. Inconsistent role filtering at the boundary.

- [ ] **J2. WS "edit" handler uses one `idx` for two different indexing schemes.**

  `routes.c:1361-1383`: `session_manager_truncate_history(c->sm, c->agent->session_id, idx)` uses DB-side indexing (no system row). Then agent-side truncation: `keep = idx < c->agent->messages_count ? idx : c->agent->messages_count - 1;` (`routes.c:1365`) frees `[keep .. messages_count)`. But `agent->messages` INCLUDES the injected system message at index 0 (per J1); DB-index `idx` and agent-index `idx` correspond to DIFFERENT messages. Chat-side truncation truncates the wrong messages when a system message is present; DB row and agent in-memory view diverge.

- [ ] **J3. `session_free(s)` indentation hazard.**

  `routes.c:1295-1296`: `}` and `session_free(s);` on the same line, inside the outer `if (s)` block but after the `if (s->messages_count > 0)` block. Semantically OK but error-prone formatting; a likely source of future bugs.

- [ ] **J4. WS history-replay path loads messages into `agent->messages` but never saves.**

  `routes.c:1254-1280`: overwrites `agent->messages` with the loaded session's messages and emits history to the client, but does not call `agent_save_session` afterward — relies on the next `agent_run_streaming` to re-persist. If the client disconnects right after the history-replay frame, the agent's loaded state is un-persisted. Chat side's "load into current agent" path skips the save that "truncate"/"edit" paths perform.

- [ ] **J5. Connecting with `?session_id=X` does NOT preload `agent->messages` from the DB.**

  `routes.c:1666-1690`: the WS chat init loads the session and emits a `history` frame to the client, but `agent->messages` stays empty. Subsequent `agent_run_streaming` will `agent_save_session` against an empty in-memory state, overwriting the DB row's existing messages with just the new user message. Connecting to an existing session via the WS query-string path silently wipes its history on the first turn. Major chat->DB state corruption.

---

## Highest-impact discrepancies (summary)

1. **[ ] J5** — reconnecting to an existing session silently wipes its DB history on first turn.
2. **[ ] C13** — encryption failures silently replace blobs with NULL while reporting success: irreversible silent data loss.
3. **[ ] C5 + [ ] C4** — decryption/parse failures silently masked, then save-back of empty defaults destroys originals.
4. **[ ] J2** — WS edit truncates the wrong messages when a system message is present.
5. **[ ] C3** — NULL deref in `session_deserialize_messages` `tool_calls` calloc-failure path (AGENTS.md §10 pattern).
6. **[ ] C10 + [ ] E1 + [ ] E2** — non-transactional load-modify-save cycles; lost updates and half-applied password migrations.
7. **[ ] A4 + [ ] A5 + [ ] A6 + [ ] C9** — round-trip loses `id`, `timestamp`, `tool_calls.result_content/result_error`.
8. **[ ] A1 + [ ] F1** — `title` plaintext despite docs; `str_dup` results unchecked.
9. **[ ] A8 + [ ] A9 + [ ] A10 + [ ] A11** — `ARCHITECTURE.md` migration/env-var/salt/first-run contracts all diverge from implementation.
10. **[ ] C6 + [ ] C7** — empty-messages save is skipped (stale DB) and `agent->session_id` silently remints when row missing.

---

## Verification convention

- `[ ]` open / not yet addressed
- `[~]` in progress / partially fixed
- `[x]` resolved and verified under sanitizer/test
- `[!]` blocked or needs architectural decision

Each fix should be demonstrated via:
1. Failing Check test before fix; passing after.
2. For memory-related issues: clean ASan/UBSan/Valgrind run.
3. `git stash` + rerun old behavior to confirm the bug reproduces pre-fix and is gone post-fix (AGENTS.md §7).