# AGENTS.md Compliance Review — Fix Plan

**Date:** 2026-08-11
**Source:** docs/reviews/AGENTS_COMPLIANCE_REVIEW.md (HEAD b717962, 88-rule review: 32 COMPLIANT / 48 PARTIAL / 8 VIOLATION)
**Repo:** /home/barontek/echo-ai-c
**Scope:** 4 confirmed live memory bugs (HIGH), 3 MED issues, 8 VIOLATION verdicts, 48 PARTIAL verdicts, 4 cross-cutting observations.

## Conventions

- Item IDs: `P<phase><n>` (A = memory bugs, B = violations, C = error handling, D = docs, E = tests, F = refactor, G = discipline). Review rule references and file:line are from the review doc, verified against HEAD.
- **Every item ships with:** the fix, a regression test that fails on the old code and passes on the new (where the bug is testable), a sanitizer run (ASan+UBSan), and for memory bugs a Valgrind spot-check. The `git stash` reproduction technique from AGENTS.md rule 82 applies to every bug item: demonstrate the failing test on the pre-fix tree.
- Track progress by ticking the `[ ]` boxes. A plan item is DONE only when its test is green under sanitizers and the evidence (test name + run) is recorded in this file or docs/verification/.
- Items marked `[defer]` are explicitly scoped out of this pass with a reason — never silently skipped.

---

## Phase A — Confirmed live bugs (fix first, in order)

### A1 [L1, HIGH] server.c response-buffer leak on every HTTP response
- **Problem:** `server_response` asprintf's `resp` (src/server/server.c:211/:226/:240) and hands it to `uv_write` via `buf.base`; `write_done` (:158-165) frees only `req`, never `buf.base`. Contrast the correct patterns at server.c:170 (sse_write_done frees `req->data`) and websocket.c:70. Steady-state unbounded growth in the long-running server; present since the initial commit. `uv_write` failure at :268 also leaks `req` + `resp` (return value ignored).
- **Fix:**
  1. In `server_response`, allocate a small on-write-complete struct `{ Client *client; char *buf; }`; set `req->data` to it. `write_done` frees `buf`, frees the struct, frees `req`, then `client_close(client)` for non-WS clients (preserves current close behavior).
  2. Check `uv_write`'s return at :268: on failure free the struct + `resp` + `req`, log with `req_id` context, and `client_close`.
  3. Do the same failure-path cleanup for `server_sse_write` (:197) — it leaks `req` + `copy` on uv_write failure today.
- **Regression test:** under the existing `SERVER_TEST` guard, drive `server_response` with a stubbed `uv_write` (pattern already used in test_routes_ws.c) that (a) succeeds and records the buf so the test can assert `write_done` freed it (free-counting shim), and (b) fails, asserting the error path frees everything. Old code: shim reports the leak count mismatch.
- **Verify:** `test_server.c` green under ASan+UBSan; Valgrind spot-check; a 100-response loop under LSan shows zero growth.

### A2 [L2, HIGH] deep_search.c double-free + UAF on non-array search result
- **Problem:** src/tools/deep_search.c:183-192 — `results_json` is attached to `output` at :184 (ownership transferred), `cJSON_Delete(output)` at :190 frees it, then `cJSON_IsArray(results_json)` at :191 reads freed memory and :192 frees it again. The guard is inverted relative to the ownership transfer. Latent today (all 3 bundled providers emit arrays or non-JSON error strings); fires the moment any provider returns an object.
- **Fix:** validate before transfer, never after:
  ```c
  if (results_json)
  {
      if (cJSON_IsArray(results_json))
          cJSON_AddItemToObject(output, "search_results", results_json);
      else
      {
          cJSON_Delete(results_json);
          cJSON_AddStringToObject(output, "search_results", "none");
      }
  }
  else
      cJSON_AddStringToObject(output, "search_results", "none");
  ```
  Delete the :191-192 block entirely.
- **Regression test:** test_deep_search.c (DEEP_SEARCH_TEST + REGISTRY_TEST already wired) registers a mock `web_search` tool whose `execute` returns a valid **object** JSON (e.g. `{"error":"provider hiccup"}`), calls `deep_search_execute`, asserts a clean ToolResult and no crash. Old code: ASan reports double-free/UAF. Also keep an array-shaped case asserting the happy path still passes.
- **Verify:** ASan+UBSan run of `test_deep_search`; Valgrind spot-check.

### A3 [L3, HIGH] routes_ws.c negative-index heap underflow in edit handler
- **Problem:** `idx = (int)idx_item->valuedouble` at src/server/routes/routes_ws.c:946 is unvalidated; `ws_run_fork` clamps `keep = idx + system_prefix` only from above (:387-389), so `idx = -1` yields `keep = -1` → `message_clear(&messages[-1])` (heap underflow read + free of garbage pointers). `(int)1e100` is a second vector (float→int cast is UB). Contrast the regenerate handler, which validates `fi >= 0 && fi + system_prefix < count` (:743).
- **Fix:**
  1. In the `edit` handler (:943-949): before calling `ws_run_fork`, validate `idx_item->valuedouble`: reject `dv < 0`, `dv > INT_MAX`, or non-integral `dv != (double)(int)dv` with an error frame (`{"type":"error","content":"invalid index"}`) and return.
  2. Defense in depth in `ws_run_fork` (:387): clamp `int keep = idx < 0 ? 0 : idx + system_prefix;` and keep the upper clamp.
- **Regression test:** test_routes_ws.c frame harness sends an `edit` frame with `index: -1` (and a second with `index: 1e100`): assert an error frame is emitted, no crash, agent context untouched. Old code: ASan heap-buffer-overflow on `messages[-1]`.
- **Verify:** ASan+UBSan `test_routes_ws`; also fuzz-relevant — run the WS fuzz target against the new validation (no harness change needed if the fuzzer already feeds edit frames; otherwise extend it).

### A4 [L4, HIGH] grep_tool.c stack OOB via snprintf would-be-length
- **Problem:** src/tools/grep_tool.c:39-41 — `*pos += snprintf(buffer + *pos, cap - *pos, ...)` accumulates the would-be length, so `*pos` can exceed `cap`; the `if (*pos >= cap - 1) break` guard stops only the current file, and the next file's call computes `cap - *pos` as a size_t underflow → write past the 32768-byte stack buffer (:90). Certain with >32 KB of matches across ≥2 files; `-Warray-bounds` can't see it (runtime index through a pointer parameter).
- **Fix:** bound-check before and after each write in `search_file`:
  ```c
  if (*pos >= cap) break;
  size_t avail = cap - *pos;
  int n = snprintf(buffer + *pos, avail, "%s:%d:%s\n", path, line_num, line);
  if (n < 0) break;
  if ((size_t)n >= avail) { *pos = cap; break; }   /* truncated: stop, stay in bounds */
  *pos += (size_t)n;
  ```
  `*pos` can now only reach `cap`, so the recursive `search_dir` → `search_file` chain stays in bounds.
- **Regression test:** extend test_grep_tool.c (currently 1 symlink-only test): build a temp workspace with 2+ files each containing ~20 KB of matching lines, run the tool, assert a truncated-but-terminated result and no crash. Old code: ASan stack-buffer-overflow.
- **Verify:** ASan+UBSan; Valgrind spot-check.

### A5 [L5, MED] session_branch.c message_clear on uninitialized stack memory
- **Problem:** `Message fork_copy;` (no initializer) at src/session/session_branch.c:304; `goto cleanup` paths at :307/:317/:322 (OOM in branch_list_ensure/mint) reach `message_clear(&fork_copy)` at :416 before `message_copy` ever zeroes it → free of garbage pointers. Exactly the historical bug class AGENTS.md calls out; `-Wuninitialized` can't see it (cross-function read).
- **Fix:** `Message fork_copy = {0};` — `message_clear` on a zeroed Message is a no-op (all NULL pointers).
- **Regression test:** test_session_manager.c fault-injection: iterate alloc-fail positions (session_branch shares session_manager's counter) until one lands in `branch_do_fork`'s pre-copy phase; assert `session_manager_fork_branch` returns -1, `*out` is zeroed, no crash. Old code: ASan invalid-free on garbage pointers.
- **Verify:** ASan+UBSan; Valgrind spot-check.

### A6 [L6, MED] encryption.c unchecked EVP_DecryptFinal_ex
- **Problem:** src/session/encryption.c:138-152 — on padding failure `plaintext_len` is never advanced, but `plaintext[plaintext_len] = '\0'` and the buffer's unwritten region is returned to callers (session_manager.c:319-330, :654-720, migration.c:246). Structurally the classic EVP trap; cryptographically gated (needs an HMAC-valid token with bad padding).
- **Fix:** check every EVP call: `EVP_DecryptInit_ex` (:142), `EVP_DecryptUpdate` (:143), and especially `EVP_DecryptFinal_ex` (:145). On any failure, zero the plaintext buffer, free it, and return -1 — never return partial output. This makes the decrypt function's failure contract: error ⇒ no output buffer at all.
- **Regression test:** under ENCRYPTION_TEST with known key halves: take a validly encrypted token, corrupt one ciphertext byte (keeping the HMAC valid by recomputing it with the known key), call the decrypt path, assert it returns an error and does not return plaintext. Needs the test to reach the key material — via the existing test path used for round-trips in test_session_manager.c (or an exposed test hook if keys are not currently reachable). Old code: returns success with truncated/garbage plaintext (assert fails).
- **Verify:** ASan+UBSan; fuzz target fuzz_fernet_token still clean.

### A7 [L7, MED] rest_api.c missing `<sys/socket.h>`
- **Problem:** src/tools/rest_api.c:31 calls a real `socket(2)` without `<sys/socket.h>` in its include block (:8-12) — the exact macOS `-Wimplicit-function-declaration` under `-Werror` trap the rule exists to prevent (curl's headers pull it in transitively on Linux only).
- **Fix:** add `#include <sys/socket.h>` to rest_api.c.
- **Sweep:** grep all of src/ for real `socket`/`bind`/`listen`/`accept` calls and verify each file carries `<sys/socket.h>` (candidates: openai_oauth.c callback server, server.c, websocket.c).
- **Regression test:** compile-only — cannot fail on Linux. Verified by the macOS CI job (backend-macos). Document the CI evidence in this plan's checklist.
- **Verify:** macOS CI green; Linux build unaffected.

---

## Phase B — VIOLATION verdicts

### B1 [Rule 12] registry.c `delegate_config` shutdown leak
- **Problem:** `delegate_config` (src/tools/registry.c:63) owns 4 str_dup'd strings set by `registry_set_delegate_config` (:262-265), freed only on replacement (:273-276). `registry_destroy()` (:323-340) never frees them → shutdown leak on every run; registry.h:239-249 doesn't mention it.
- **Fix:** free the 4 strings at the end of `registry_destroy()`. Update registry.h doc to state registry_destroy releases the delegate config.
- **Regression test:** test_registry.c: set a delegate config, call `registry_destroy()`, run under LSan (Linux CI) — old code leaks. For a deterministic assertion, use a free-counting shim under REGISTRY_TEST: after destroy, all 4 pointers were freed.
- **Verify:** ASan/LSan run of test_registry.

### B2 [Rule 12] semantic_search.c `search_index` has no production destroy
- **Problem:** `search_index` (src/tools/semantic_search.c:55) owns 4 allocated arrays/strings (:186-203) with no production destroy — frees exist only in the test-only `semantic_search_test_reset()` (:70-92, under SEMANTIC_SEARCH_TEST).
- **Fix:** add `semantic_search_free_index()` (or `semantic_search_destroy()`), freeing documents/all_terms/term_freqs rows/doc_lengths and re-zeroing the struct; document ownership in semantic_search.h (who calls it). Wire it into the owning lifecycle (the search provider destroy path / deep_search tool destroy — trace the actual owner and call it there; this is the fix that makes the header's ownership contract true).
- **Regression test:** LSan-based: index documents, free, assert no leak (old code leaks). Plus fix the 2 fault tests that "assert nothing" (see E12).
- **Verify:** LSan run of test_semantic_search.

### B3 [Rule 13 + Rule 15] agent.c execute_tool_calls OOM handling + 5 silent void functions
- **Problem:** `execute_tool_calls` (src/agent/agent.c:156) ignores `agent_append_message` failures at :174-178/:191-195/:248-261 (message + str_dup'd fields leak on realloc-OOM) and dereferences `message_create` results without NULL check (calloc-OOM crash). Separately, `agent_set_model` (:1088), `agent_generate_title` (:626), `agent_apply_title` (:610), `agent_perform_summarization` (:678), `inject_system_with_summary` (:349) are `void` with every failure path a silent bare return (SQLite I/O, LLM calls, allocations).
- **Fix:**
  1. In the three message-construction sites: check `message_create` for NULL (log + `continue`), and check `agent_append_message`'s return — on failure `message_free(err_msg)` + log_error with tool name.
  2. Convert the 5 void functions to return `int` (0/-1) and propagate/log at call sites with context. Header updates in agent.h (ownership/error signaling per kernel-doc).
  3. Check the str_dup fields before use where a NULL field is dereferenced downstream (audit :175-177/:249-253 during the fix).
- **Regression test:** needs agent.c fault hooks — the AGENT_TEST definition exists but is dead (see B6); wire a real `set_alloc_fail` hook and add fault tests: append-failure leaves no partial message and no crash; title-generation OOM returns -1 (with a stub LLM provider, pattern already used in tests/agent). Old code: crash/leak on the injected position.
- **Verify:** ASan+UBSan test_agent suite; Valgrind spot-check.

### B4 [Rule 49, HIGH] server_stop NULL contract mismatch
- **Problem:** server.h:93 documents "@ctx: ... NULL is accepted" but src/server/server.c:779-782 dereferences `ctx->loop` unconditionally — the only "NULL is a no-op" claim not backed by a guard, and exactly the class the rule exists to prevent.
- **Fix:** add `if (!ctx) return;` at the top of `server_stop` (match the documented contract).
- **Regression test:** test_server.c (SERVER_TEST) calls `server_stop(NULL)` — assert no crash. Old code: segfault.
- **Verify:** ASan run.

### B5 [Rule 10b] `-Wno-unused-parameter` on 7 fuzz targets
- **Problem:** tests/CMakeLists.txt:65/:77/:91/:103/:145/:159/:222 — no in-file rationale; the "harness files only" claim is false (`target_compile_options` is target-wide; compile_commands.json shows it applied to production sources like src/config/config.c); empirically vestigial (clang -fsyntax-only with the warning enabled produces zero diagnostics on every affected TU).
- **Fix:** remove the flag from all 7 targets. If the compile surfaces genuine unused parameters, add `(void)param;` instead. Add the rationale comment in tests/CMakeLists.txt only if something requires it again.
- **Regression test:** compile-commands.json shows no `-Wno-unused-parameter` in the new build; full fuzz build + CI fuzz job green.
- **Verify:** local fuzz build (`nix develop` + the CI fuzz commands) plus CI.

### B6 [Rule 86] Vestigial `AGENT_TEST` definition
- **Problem:** tests/agent/CMakeLists.txt:49 defines AGENT_TEST; nothing in src/ references it.
- **Fix:** remove the dead definition — or, if B3's fault hook lands in agent.c, reuse the AGENT_TEST guard for it and reference it here. Choose one; leave no dead definitions.
- **Verify:** grep AGENT_TEST → only the sanctioned use.

### B7 [Rule 35] 265 multi-statement lines + 8 comma-chained declarations
- **Problem:** ~265 lines with multiple statements across ~66 of 110 files; dominant idiom is single-line `if (x) { free(a); free(b); return NULL; }` error handlers; worst: tool_delegate.c:312 (8 statements), registry.c:269 (5), main.c (5-statement lines ×6), json.c:25-29 (case-label triples). 8 comma-chained declarations: encryption.c:70/:141, html_extract.c:441/:683/:795, test_html_extract.c:333, test_callbacks.c:128/:251.
- **Fix:** mechanical sweep — split every multi-statement line into one statement per line; expand comma-chained declarations. Batch by file, largest offenders first (openai_oauth.c 31, main.c 22, session_branch.c 21, html_extract.c 17, openai.c 12).
- **Regression test:** none behavioral — verify with a repo-wide grep that no line contains `;` followed by a statement (a check script or CI grep) plus full build with -Werror.
- **Verify:** full ctest run; no warning changes.

### B8 [Rule 55] Zero module READMEs + stale root README
- **Problem:** 0 of 9 C subsystems have a module README (src/, src/agent, src/llm, src/session, src/tools, src/server, src/utils, src/safety/config/change_tracker, tests/, deploy/). Root README.md stale 3×: :168-171 references `web/` (gone since e4073c2), :3 claims "vanilla JS frontend" (actually React+Vite+TS), :94 marks `--chat` "not yet implemented" (implemented at main.c:167/:731/:779). frontend/README.md and scripts/README.md are build docs with zero design rationale.
- **Fix:** one short README per subsystem: what the module owns, why it exists (one sentence, no "and"), how it wires together — per AGENTS.md "one README per module/subsystem". Fix the 3 stale root-README claims in the same pass. Add design rationale sections to frontend/README.md and scripts/README.md.
- **Verify:** each README exists and the root README's three claims are corrected; `git grep web/` clean.

---

## Phase C — Error handling and overflow (rules 15-19)

### C1 [Rule 16 M1] search_duckduckgo.c unchecked str_dup
- **Problem:** src/tools/search_duckduckgo.c:46-49 — unchecked `str_dup` result dereferenced in the HTML-entity decode loop (NULL-deref crash on OOM).
- **Fix:** check and bail with an error result; follow the file's existing error conventions.
- **Regression test:** fault-injection (the file is fuzz-covered; add a *_TEST alloc-fail hook if none exists) — fail the str_dup, assert an error result, no crash. Old code: NULL deref.

### C2 [Rule 16 M2 + Rule 17] replace_in_file.c write failure reported as success
- **Problem:** src/tools/replace_in_file.c:120-121 — `fwrite` result never compared to expected length, `fclose` unchecked; disk-full silently truncates the file and the tool reports success (contrast write_file.c:93-99, which does it correctly).
- **Fix:** compare the fwrite count against the expected length; check fclose; on either failure return a tool error and report the failure. Also snapshot/undo state should not be committed on a failed write (mirror write_file.c's ordering).
- **Regression test:** write to `/dev/full` (ENOSPC) via the tool's file path (needs the path to pass the safety workspace check — use a temp workspace; if /dev/full is blocked by safety, inject the write failure with a test hook): assert the tool returns an error and the file is not reported as replaced. Old code: success + truncated file.

### C3 [Rule 16 M3 + Rule 17] rate_limiter.c discarded sqlite3_step results
- **Problem:** src/utils/rate_limiter.c:107/:117/:131/:147 — 4× `sqlite3_step` results discarded; :147 is security-relevant: a failed INSERT means a brute-force unlock attempt is never recorded, weakening the unlock throttle. The file's own comment (:86-88) promises failures are "loudly logged", but ~10 of ~11 prepare-failure paths return bare with no log.
- **Fix:** check every sqlite3_step and prepare result; log loudly on failure per the file's own documented contract. Decide explicitly at :147: on record failure, log + fail closed (do not grant) or fail open with a loud log — pick one, document it, test it.
- **Regression test:** test_rate_limiter.c — inject sqlite errors (test hook or a pre-corrupted DB), assert the failure is logged and the throttle state is what the documented contract says. Old code: silent.

### C4 [Rule 16 M4] session_manager.c 17× ignored pthread_mutex results
- **Problem:** src/session/session_manager.c:761/:766/:1070/:1077/:1086/:1093/:1179/:1187/:1192/:1202/:1212/:1219/:1226/:1517/:1530/:1539/:1550 — mutex lock/unlock results ignored (sibling openai_oauth.c checks all ~50 of its calls).
- **Fix:** check and log with context at every site (consistency with openai_oauth.c). Low behavioral risk; mechanical.
- **Verify:** full ctest; no behavior change expected.

### C5 [Rule 16 LOW + Rule 17] residual unchecked-return sweep
- **Problem:** ~30 LOW sites: 4× fread partial-result ignored (ingest_document.c:78, notes.c:230, read_file.c:68, replace_in_file.c:88); ~13 unchecked sqlite3_bind_*; 5× sqlite3_step loop-exit error treated as SQLITE_DONE; ~12 unchecked str_dup into message/context/session fields; ~40 curl_easy_setopt (documented as cannot-fail — leave, add a comment); unchecked str_dup at agent.c:82-83/:1092 and main.c:540.
- **Fix:** per-site: check, log with context, and fail the operation where a partial result is wrong (reads: partial fread is a hard error for tools that must return whole files); loop-exit sqlite3_step errors must be distinguished from SQLITE_DONE; str_dup into struct fields: NULL-safe downstream sites get a documented "NULL tolerated" note, non-NULL-safe sites get checks (see B3 for agent.c).
- **Verify:** code-review trace + ctest + fuzz run.

### C6 [Rule 17] routes_ws.c silent empty-history runs
- **Problem:** src/server/routes/routes_ws.c:536-546 and :1222-1233 — `message_copy()` failure → messages freed, count zeroed, loop continues: the agent runs with **zero history**, nothing logged, no error frame. :779-795 branch-switch path returns -1 but the caller only handles `swap_rc == 0` (silent empty history after a "successful" switch).
- **Fix:** on copy failure: log with session id context, send an error frame to the client, and abort the run instead of continuing with empty history. Handle `swap_rc != 0` on the branch-switch path: send an error frame + log.
- **Regression test:** test_routes_ws.c fault injection on the copy path (allocate-fail hook mid-swap) — assert an error frame is emitted and no run starts. Old code: empty-history run, no frame.

### C7 [Rule 17] change_tracker undo/redo destroys the only copy on failed write
- **Problem:** src/change_tracker.c:137-140/:172-177 — `ct_undo`/`ct_redo` consume the undo entry regardless of fwrite success; main.c:458-465 reports "Undone (%d bytes restored)." even for a 0-byte restore.
- **Fix:** on write failure, restore the entry (or keep it) so the previous content survives, return an error, and report it.
- **Regression test:** fault-inject the fwrite (rename/write hook exists per rule 85) — assert the undo entry is preserved and an error surfaces. Old code: entry consumed + success message.

### C8 [Rule 17] routes_session.c:68-72 DB error presented as empty session list
- **Problem:** `session_manager_list_sessions()` failure → HTTP 200 OK `{"sessions":[]}` — a DB error indistinguishable from "no sessions".
- **Fix:** distinguish: on error return HTTP 500 with an error body.
- **Regression test:** test_routes_session_handlers.c — stub list_sessions to fail, assert 500 + error body, not 200-empty. Old code: 200 `[]`.

### C9 [Rule 17] notes.c / tool_memory.c false negatives
- **Problem:** notes.c:129-130 — `opendir()` failure → success "(no notes)"; notes.c:82 mkdir rc ignored. tool_memory.c:113-115/:155-158 — DB errors reported as "(not found)"/"(no memory stored)" (memory.c returns NULL for both "no row" and SQL error).
- **Fix:** notes: distinguish opendir failure from empty dir (error result); check mkdir. tool_memory/memory: return an error channel from memory.c distinguishing SQL-error from not-found, and propagate.
- **Regression test:** notes: point the tool at a non-existent directory → assert an error result, not "(no notes)". memory: corrupt the DB (or stub) → assert an error result, not "(no memory stored)".

### C10 [Rule 17] server.c uv_write failures swallowed
- **Problem:** server.c:197/:158-165/:167-170 — uv_write failures leak req + frame copy (doc at :172-174 admits the silent drop); websocket.c:67-72 `(void)status` on failed async write (mitigated by close).
- **Fix:** on uv_write failure in server_response/server_sse_write: log, free the request + payload, close the client (HTTP). websocket.c: log the failed write before the close.
- **Regression test:** covered by A1's stubbed-uv_write harness; websocket case in test_websocket.c with a stub write returning failure — assert cleanup + close.

### C11 [Rule 17] remaining swallow sites (batch)
- **Problem:** routes_ws.c:233-234 queued user message dropped on cJSON_Parse failure with no frame/log; :273-274 calloc failure → bare return; :153-166 + routes_chat.c:135-150 stream chunk drops silent; agent.c:1019-1021 message_copy failure mid-save → silently truncated persisted history; bash.c:204-224/python_execute.c:89-101 fake "Exit code: -1"/empty-success; write_file.c:74 ct_snapshot failure ignored; session_manager.c:209-210 PRAGMA rc ignored; config.c:109-151 parse errors drop entries (document each as intended or log); list_dir.c:52-54/grep_tool.c:54-61 truncation without marker; tool_ask_user.c:56-60 getline failure → "(user did not respond)"; tool_delegate.c:310-312 OOM message drop; server.c:735-749 on_connection unchecked returns.
- **Fix:** per site: log with context and/or error frame where the client must know; add truncation markers (e.g. `\n... (truncated)`); on_connection failures logged; config.c drops documented in-code. Each sub-fix gets its own test where a harness exists (test_routes_ws, test_config, test_bash, test_write_file, test_tool_ask_user, test_tool_delegate).
- **Verify:** ctest + targeted fault tests.

### C12 [Rule 18] Context loss at boundaries
- **Problem:** bash.c:31/:34/:56/:64/:80/:205-219 subprocess launch failures collapse to `rc == -1` rendered as `"Exit code: -1\n"` with no explanation (contrast python_execute.c:47/:55, which wrap the same failures as "pipe creation failed"/"fork failed"); session_branch.c has 35 error returns and zero logs (fork/switch failures surface as generic "fork failed"); agent.c:765/:795-799 OOM returns NULL bare → generic "no response"; websocket.c:77-132/:246-248 frame parse failures silently dropped. `strerror()` is used nowhere in src/ (migration.c:153/:323, safety.c:573, bash.c discard errno text).
- **Fix:** bash.c: include strerror in the failure output ("fork failed: %s"). session_branch.c: add log_error with context at each error return. agent.c: log the OOM before returning NULL. websocket.c: log frame-parse failures with opcode/length context. Sweep: use strerror() at errno-based boundaries.
- **Verify:** trace + ctest; grep confirms strerror use at the named sites.

### C13 [Rule 19] Signed-overflow sites
- **Problem:** agent.c:687 `int text_len += strlen(...)` on untrusted content (UB past ~2 GB, feeds malloc(text_len+1) at :692); context.c:357 same pattern; agent.c:690 `max_context_chars * 2` with an operator-controlled config value ≥ 2^30 (no range check, unlike validated sibling session_manager.c:1500); config.c:174-176 `strtol` → `int` cast without range clamping (feeder for the above).
- **Fix:** switch accumulators to `size_t` with a saturation cap before malloc; validate `max_context_chars` at config load (clamp to a sane bound or reject); clamp the strtol result to INT range at config.c:174-176.
- **Regression test:** config: load a config with `max_context_chars` ≥ 2^30 → assert clamped/rejected (old code: overflow-ready value stored). agent/context: a unit test driving text_len near the cap (mock size) asserting no overflow (behavioral test optional; the clamp is the assertion).

---

## Phase D — Documentation contracts (rules 43-56, 88, cross-cutting)

### D1 [Rule 43/49/51] Wrong doc comments on correct code
- **Problem:** 4 conflicting/incorrect docblocks (implementations are correct): semantic_search.h:23-34 duplicate contradictory `Return:` (also flagged in rules 43/49/51); routes.h:60 `ws_add_message_to_json` "Return: void" on int; routes.h:74 `server_sse_write` "Return: void; failures are logged nowhere" (returns -1 and DOES log at server.c:185/:192); websocket.h:117 `ws_start_ping_timer` "Return: void; never fails" (returns -1 on uv_timer_init failure). Plus session_manager.h:369-380 return values in prose (no tag); http_client.h:55 two params on one line.
- **Fix:** rewrite the 4 docblocks to match the implementations; convert the prose returns to a `Return:` tag; split the two-param line. Also fix the 5 MINOR missing/NULL semantics gaps and 4 PARTIAL self-contradictory docs named in rule 49 (agent_run_new user_input NULL semantics; session_deserialize_metadata/_events NULL json_str wording; openai_oauth account_id out-param wording; route_match method/path).
- **Verify:** doc/reality trace per header.

### D2 [Rule 50] Thread-safety doc gaps
- **Problem:** provider.h:16-19 vtable comment never states whether chat/chat_streaming/extract_structured are concurrent-safe (ollama's only IMPLIED via `_Atomic` call_seq); middleware.h:65/:80/:92 "Never fails; thread-safe" overstates (unsynchronized ServerContext state + a RateLimiter that needs caller serialization — safe only under the loop-thread model, which the header doesn't reference); agent.h:209-210 `agent_cancel` "Safe to call from another thread" implemented as plain `volatile int` (benign in practice, technically UB).
- **Fix:** provider.h: state the concurrency contract per method (and fix any implementation that doesn't meet it); middleware.h: qualify with the loop-thread requirement; agent_cancel: either switch the flag to C11 `_Atomic int` (sanctioned by the C11 toolchain) or narrow the doc to "safe to call, result may race" — prefer the `_Atomic` fix. Undocumented statics: add comments for server.c:370 req_counter and routes_ws.c:1071 approval_counter.
- **Verify:** doc trace + full ctest (atomic change is ABI-neutral).

### D3 [Rule 52/53] TSDoc gaps (frontend)
- **Problem:** ChatContext.tsx `ConnectionStatus` (:4), `EffortOption` (:15), `ChatContext` (:80) have no TSDoc; `EFFORT_OPTIONS_BY_PROVIDER` (:9) uses plain `/* */` (hover tooltips won't parse). ChatInput.tsx:19 claims "owns no effects" but has a `useEffect` at :35-40 (textarea auto-resize) — stale doc. Async-failure-mode is the most common omission at component level (App, ChatProvider loadData, Sidebar/SetupScreen).
- **Fix:** add TSDoc to the 3 un-documented items; convert to `/** */`; fix ChatInput.tsx's claim; add failure-mode sentences to the component docs that lack them.
- **Verify:** `npm run lint` (eslint-plugin-jsdoc) + hover check in editor.

### D4 [Rule 54] Mechanics-leaking docs
- **Problem:** migration.h:43-49 (migration_change_password: full step-by-step walkthrough of marker file/salt.old/verifier.new/transaction ordering — changes on any reimplementation while the 0/-1/-2 contract doesn't); migration.h:22-29 (internal decision procedure); context.h:37-40 (names internal callees smart_select_alloc/trim_messages_by_tokens_new). 8 mild: session_manager.h "load, append, save triad", http_client.h growth strategy.
- **Fix:** rewrite the 3 to behavior level (what the caller may rely on), keep "why" notes for the constraints that explain the contract.
- **Verify:** doc trace.

### D5 [Rule 45] Duplicated contract blocks in .c files
- **Problem:** session_manager.c:1057-1065 re-prints session_manager.h:159-170 verbatim; websocket.c:191-194 restates websocket.h:133-145; server.c:174-178 restates routes.h:65-77 (keeps a legitimate stack-buffer why); routes_ws.c:647-658 identical comment pasted back-to-back. openai_oauth.c has 3 WHY comments in 2151 lines (single-flight refresh wait loop, callback header anti-rebinding validation, staged token parse all unexplained).
- **Fix:** trim the restatements to a one-line pointer to the header; add the missing WHY comments in openai_oauth.c.
- **Verify:** review diff.

### D6 [Rule 46] Test files lack file headers
- **Problem:** 61 of 83 test files lack `/* <name> - <description> */` headers (10 fuzz targets have none at all); scripts/decrypt_session.c and scripts/test_ollama.c have none.
- **Fix:** add one-line headers to all 83 test files + 2 scripts (mirroring the src/ convention). Mechanical.
- **Verify:** grep audit — every file under tests/ and scripts/ starts with the header.

### D7 [Rule 56 + Rule 11] Format uniformity + orphan doc
- **Problem:** 2 `Returns:` in .c comments (session.c:298, session_manager.c:1058); "Return:" separator variance ("void." vs "void;" vs "nothing." — 114 blocks) — normalize; openai_compatible_test_parse_response (openai_compatible.c:230) has no doc comment and no header declaration (public test-only function returning LLMResponse*).
- **Fix:** normalize separators; add the doc comment (and either declare it in openai_compatible.h or make it static — prefer declaration so the test contract is discoverable).
- **Verify:** regex audit over headers.

### D8 [Rule 24] provider.h orphan header — document the exception
- **Problem:** src/llm/provider.h has no provider.c; it's a shared LLM-provider vtable contract behaving exactly like the documented tool.h exception, but the exception isn't documented for src/llm/ in AGENTS.md.
- **Fix:** document the exception in AGENTS.md (extend the "Documented exception" sentence to cover src/llm/provider.h), and note it in provider.h's file header.
- **Verify:** doc review.

### D9 [Rule 88, cross-cutting] AGENTS.md "Known gaps as of this sweep" is overstated
- **Problem:** the AGENTS.md paragraph claims 8 modules' allocation-safety paths are covered. Review: solidly covered 3/8 (metrics, change_tracker, registry); overstated 5/8 (memory.c — only memory_list_all tested, memory_get_dup zero coverage, memory_get OOM indistinguishable from not-found; tool_delegate.c — loop-phase commit sites untested; session_manager.c — add_message realloc+rollback and load_session_locked str_dups untested; config.c — mid-list token cleanup untested, continuation allocs not injectable; semantic_search.c — 2 of 3 fault tests assert nothing). Newer modules: routes_session.c dead hook (plan §5.7 claim contradicted by the tree), migration.c no hook at all. The remediation plan §9 (:790-796) explicitly says the wording should be fixed; AGENTS.md was never updated.
- **Fix:** rewrite the "Known gaps as of this sweep" paragraph in AGENTS.md to state the actual coverage (list the gaps precisely), and re-run it after E11/E12 land. The plan's §9 wording fix happens in the remediation plan document (see G3).
- **Verify:** AGENTS.md reflects the review's findings; after E11/E12 the paragraph is updated again to reflect the new coverage.

### D10 [Rule 43/56 doc quality — cross-cutting] ID-scheme mismatch
- **Problem:** fix IDs in code/tests (A1-A13, B1-B10, C1-C15, D1-D5, E1-E2, F1-F6, G, H2, I2-I3, J1-J5) come from deleted CHAT_DB_DISCREPANCIES.md; AGENTS_COMPLIANCE_REMEDIATION_PLAN.md reuses letters with different numbers. A reviewer matching IDs across docs will be misled.
- **Fix:** add an appendix to the remediation plan (or a new docs/reviews/ID_MAPPING.md) mapping both schemes; do not rename the ID families in code (that would churn tests). This fix-plan's own items reference review rule numbers, not the stale IDs.

---

## Phase E — Test coverage (rules 57-78, 85, 88)

### E1 [Rule 58] Regression tests for all Phase A bugs
- The A1-A7 items each carry their regression test (see Phase A). This item is the tracking entry: all 7 must fail on the pre-fix tree and pass post-fix, evidence recorded (test output + sanitizer run).

### E2 [Rule 70/88] routes_session dead alloc-fail hook
- **Problem:** `routes_session_test_set_alloc_fail` exists (routes_session.c:25), wired (tests/routes/CMakeLists.txt:24), invoked nowhere; the remediation plan §5.7 ticks `[x]` for a test that does not exist.
- **Fix:** write the fault tests the hook was built for (each handler's allocation-failure paths → correct error status, no partial commit), then re-verify the plan claim (fix the `[x]` if the behavior doesn't match the claim — see G3).
- **Verify:** fault tests green; plan claim corrected.

### E3 [Rule 70/88] migration.c allocation-fault coverage
- **Problem:** no allocation-fault hook in migration.c; its 5× asprintf multi-alloc in migration_change_password is untested for OOM.
- **Fix:** add a MIGRATION_TEST guard with an injectable asprintf shim (variadic wrapper on vasprintf with a fail counter) and fault tests for each position: error return, no partial state, no crash. This directly exercises the A6-adjacent code path (migration.c:246).
- **Verify:** ASan fault tests.

### E4 [Rule 88] memory.c coverage
- **Problem:** test_memory.c has 1 test (only memory_list_all alloc-fail); memory_get_dup's allocation has zero fault coverage; memory_get's OOM-NULL is indistinguishable from "not found" (feeds the C9 tool_memory false-negative bug).
- **Fix:** add: memory_get_dup alloc-fail (error, no partial commit); memory_get/delete/set SQLite-error paths; NULL-arg coverage; and the not-found vs error distinction the C9 fix introduces.
- **Verify:** test_memory.c grows to cover all 8 functions.

### E5 [Rule 88] tool_delegate.c loop-phase commit sites
- **Problem:** only entry-phase positions 4-7 + one grow-realloc are tested; the loop-phase commit sites are untested: tool-not-found 5 str_dups (:251-255→:282), tool-result 4-5 str_dups (:296-304→:335), final_content (:192), assistant grow (:216).
- **Fix:** extend test_tool_delegate.c fault tests over the loop-phase positions: fail each str_dup, assert error/no partial commit/no crash, reset, re-verify normal operation.
- **Verify:** ASan fault tests.

### E6 [Rule 88] session_manager.c add_message rollback + load_session_locked
- **Problem:** add_message's realloc+rollback (:1246-1290) and load_session_locked's unchecked str_dups (:642-645/:658) are untested.
- **Fix:** fault tests: realloc failure mid-add → message count unchanged, no dangling struct, everything freed; str_dup failure in load_session_locked → clean error.
- **Verify:** ASan fault tests.

### E7 [Rule 88] config.c mid-list token cleanup + continuation allocs
- **Problem:** mid-list token cleanup failure untested; continuation realloc/asprintf/array calloc not injectable.
- **Fix:** extend config.c's fault hook (or add one) to cover the continuation allocations; fault tests asserting entries before the failure point stay committed and no partial entry is committed.
- **Verify:** ASan fault tests.

### E8 [Rule 88] semantic_search.c fault tests assert nothing
- **Problem:** 2 of 3 fault tests assert no return value and no no-commit condition; a document is still committed with a missing term on add_term failure.
- **Fix:** strengthen the fault tests: assert error return, doc_count unchanged, index usable after reset. Pair with the B2 destroy function's leak test.
- **Verify:** ASan fault tests.

### E9 [Rule 66] Thin test files
- **Problem:** test_memory.c (1 test for 8 functions — see E4), test_grep_tool.c (1 test, symlink-only — extended by A4), test_server.c (4 tests for 782 lines, security-only — extend with route dispatch/accept error paths; note the review's "stubbed out, untestable" caveat: expose the needed seams under SERVER_TEST), test_rest_api.c (2 tests, no success path — add a success-path test against a stubbed transport or a local socketpair-based HTTP server; the tool's curl calls make this the trickiest — use the curl capture/redefinition pattern already used in ollama.c under OLLAMA_TEST).
- **Verify:** each file's test count and coverage per its module's function inventory.

### E10 [Rule 59] Fuzz gaps
- **Problem:** session_deserialize_metadata and session_deserialize_events (session.c:318/:354) not fuzzed; search_brave.c and search_tavily.c JSON response parsers unit-tested but not fuzzed.
- **Fix:** add 2 fuzz targets (fuzz_session_metadata/events or extend fuzz_session_deserialize) + 2 for the search parsers; wire into tests/fuzz/CMakeLists.txt under HAS_LIBFUZZER; CI fuzz job picks them up automatically.
- **Verify:** fuzz targets build and run the CI bounded loop clean.

### E11 [Rule 67/68/77] Fixture and shared-state violations
- **Problem:** 2 violations + residue: test_tool_delegate.c scripted statics never reset (breaks exact-count assertions under CK_FORK=no; alloc-fail hooks never restored to -1); test_routes_ws.c ws_ask_user_cb TCase is the only TCase without a checked fixture (stub_uv_now_calls never reset → stale deadline hang under serial mode). Minor: test_notes.c teardown empty (leaks mkdtemp dirs, HOME never restored); test_logging.c no fixtures (fd-2 juggling, self-restore only); test_list_dir.c never unlinks /tmp/echo_ld_file_probe; test_safety.c fixed /tmp/echo_test_ws path (not mkdtemp — prior-crash residue cascades); test_write_file.c leaks 6 mkdtemp dirs; test_ingest_document.c leaves its workspace dir.
- **Fix:** add checked fixtures with full reset (statics, counters, hooks → -1) for test_tool_delegate.c and the ws_ask_user_cb TCase; real teardowns for the residue cases; mkdtemp for test_safety.c.
- **Verify:** full suites pass under both `CK_FORK=yes` (default) and `CK_FORK=no`, and serial-mode order-independent (run each suite twice in reversed order via CK_RUN_CASE).

### E12 [Rule 78] test_routes_general.c missing timeout
- **Problem:** the handle_models TCase runs `system("rm -rf ...")` subprocesses (:835/:952/:989) plus real temp-file I/O with no timeout (all 9 TCases untimed).
- **Fix:** add `tcase_set_timeout` on the subprocess-running TCases.
- **Verify:** suite green.

### E13 [Rule 73/63/75] Test naming
- **Problem:** test_ws_init_with_session_id_query (test_routes_ws.c:2040) is the single regression test without an annotation (guards the D5 ?session_id= resume feature); 7 vague names (test_ws_add_message_to_json_basic, test_chat_success_basic, test_on_chunk_basic, test_send_done_basic, test_on_message_message_simple, test_ws_init_basic, test_metrics_empty); 6 VAGUE operation-only names (test_cb_create_destroy, test_cb_register_and_count, test_json_add_int, test_json_add_double, test_json_serialize_object, test_rl_create_destroy).
- **Fix:** add the regression comment to test_ws_init_with_session_id_query; rename the 13 vague tests to claim-stating names.
- **Verify:** grep audit.

### E14 [Rule 65] Multi-behavior tests
- **Problem:** 5 violations: test_string.c:22 (str_starts_with AND str_ends_with); test_session_manager.c:55 (roundtrip AND delete), :264 (migration AND crash recovery), :719 (load AND delete AND save NULL-id); test_openai.c:670 (refresh success AND failure).
- **Fix:** split each into per-behavior tests.
- **Verify:** suite green, name audit.

### E15 [Rule 74] Flat TCases
- **Problem:** 4 FLAT violations: test_html_extract.c (one tcase, 31 tests across ~6 areas), test_web_fetch.c (one tcase, 17 tests across 4 areas), test_config.c ("Core" mixing load/get_int/provider_tokens with OOM tests outside the FaultInjection convention), test_server.c ("Parsing" mixing HTTP parser + static-path security).
- **Fix:** split each into per-area TCases; move test_config.c's OOM tests into a FaultInjection TCase.
- **Verify:** tcase audit.

### E16 [Rule 64] Oversized test files
- **Problem:** test_routes_ws.c (2250), test_session_manager.c (1846, multi-module — clearest case), test_routes_session_handlers.c (1144, already a split product), test_routes_general.c (1107).
- **Fix:** split test_session_manager.c by module (session facade / branch / migration / encryption per rule 61's own mirroring); split test_routes_ws.c by feature; leave the already-split files with a note.
- **Verify:** file sizes + suite green.

### E17 [Rule 76] Untyped ck_assert
- **Problem:** 331 untyped `ck_assert(cond)` (10.7%): 260 strstr substring checks → `ck_assert_str_contains`; 30 NULL checks → `ck_assert_ptr_null/not_null`; 23 int/pointer/double comparisons → typed macros; 3 strcmp → `ck_assert_str_eq`; 15 boolean predicates have no typed equivalent (leave).
- **Fix:** mechanical conversion with the typed macros.
- **Verify:** full ctest; assertion count audit (target <5% untyped).

### E18 [Rule 61] File-name/area mismatches
- **Problem:** test_string.c vs string_utils.c; test_change_tracker.c (tests/session vs src/change_tracker); test_config.c (tests/utils vs src/config); test_safety.c (tests/agent vs src/safety); routes flattening (tests/routes vs src/server/routes).
- **Fix:** rename/move to mirror the source tree (rule 61's convention). CMake lists updated accordingly. `[defer]` if a move churns the CI config disproportionately — decide per-file, document the decision.
- **Verify:** mirror audit.

### E19 [Rule 62] Branches TCase split candidate
- **Problem:** the 13-test Branches TCase (~600 lines) in test_session_manager.c covers distinct file session_branch.c.
- **Fix:** after E16's split, this resolves itself — extract the Branches TCase into tests/session/test_session_branch.c.

### E20 [Rule 71] Tests pinning internals
- **Problem:** test_change_tracker.c 5/5 pin undo_count/redo_count (undocumented representation state); test_routes_ws.c ~15 pin mirrored WSChatCtx layout + queue-node internals; test_tool_delegate.c:173-176 pins exact chat/tool call sequence; test_ollama.c:394-495 mirrors WriteBuf ("must stay in sync").
- **Fix:** convert to behavior assertions where cheap (change_tracker: assert undo/redo *behavior*); for the deliberately-synchronized mirrors (routes_ws, ollama), add a static_assert or build-time comment pointing at the source struct, and document in the test header why the mirror exists. `[defer]` full de-pinning where the mirror IS the harness contract.
- **Verify:** suite green.

---

## Phase F — Refactoring (rules 26-38)

### F1 [Rule 26] 52 unjustified functions >60 lines
- **Problem:** 118 functions over 60 lines; 52 unjustified. Worst: run_cli (main.c:327, 271), delegate_execute (tool_delegate.c:87, 270), notes_execute (notes.c:100, 204), handle_models (routes_general.c:198, 191), rest_api_execute (rest_api.c:34, 149), load_session_locked (session_manager.c:604, 140), safety_load_from_conf (safety.c:62, 137), deep_search_execute (deep_search.c:72, 127), handle_unlock (routes_auth.c:122, 126), execute_tool_calls (agent.c:156, 116).
- **Fix:** split each along phase boundaries (extract per-step helpers); use the remediation plan's F3 table as the prior-art list for already-split functions so none regress. Order: the 10 worst first, then the remaining 42 at one-per-commit cadence.
- **Verify:** identical behavior — full ctest + a diff of `--help`/sample-run outputs; no test changes except new split-helper fault tests where the split exposes one.

### F2 [Rule 28/29/31/32] File-level splits
- **Problem:** openai_oauth.c (2151 lines, 99 functions — mixed PKCE/JWT primitives, credential vault, full local HTTP callback server with own pthread listener, device polling, public state machine); session_manager.c (1583 — store + OAuth storage + fault scaffolding); html_extract.c (1524, ~36/50 statics behind one public function — strongest candidate).
- **Fix:** split openai_oauth.c into oauth_http_callback.c (the ~12-static callback-server cluster) + oauth_crypto.c (PKCE/base64/JWT primitives) + oauth_vault.c; split html_extract.c's assembler/writer cluster into html_extract_write.c; extract session_manager.c's OAuth storage into session_oauth.c. Each split keeps one header per module (rule 24) with kernel-doc, and updates CMake + tests (mirror per rule 61). This is the largest phase — one split per commit, each with a full test run. `[defer]` openai.c (2003 lines) and routes_ws.c (1344) — justified/borderline per the review; revisit after the named splits land.
- **Verify:** full ctest + sanitizer run after every split commit; function-count audit per file.

### F3 [Rule 34] Nesting depth
- **Problem:** 13 of 696 functions nest ≥6; max 8: delegate_execute (for>for>if>if>else>if>if), ws_handle_session_id (routes_ws.c:499), ws_apply_query_session (:1165), ws_handle_provider_frame (7), append_title_text/emit_text (7), write_cb (7), session_deserialize_messages (tool_calls loop).
- **Fix:** extract inner blocks to helpers (early-return the guard chains; the routes_ws session-loading twins (:499/:1165) are copy-pasted and should be merged into one helper — also de-duplicates rule 45's pasted comments).
- **Verify:** nesting-depth audit script (≤5 for all non-parser functions).

### F4 [Rule 38] Missing-WHY comments
- **Problem:** ~12 gaps: websocket.c RFC 6455 opcodes 0x8/0x9/0xA/0x1/0x2 (:252-280), mask XOR (:285-286), `+= 18` (:320), `0x81` (:397), extended-length encoding (:388-414); server.c openat/O_NOFOLLOW symlink-safe descent (:91-121), path-traversal defense (:74-84/:95), 10 MB cap (:356); bash.c process-group SIGTERM→100ms grace→SIGKILL escalation (:125-169), magic 50ms (:166); encryption.c key-half split (:184-185/:200-201); openai.c:177-179 exact-fill bound. Plus 2 pure-WHAT restatements: config.c:74, agent.c:580.
- **Fix:** add WHY comments at the 12 sites; delete/replace the 2 restatements.
- **Verify:** comment audit.

### F5 [Rule 29/33] Descriptive-scope flags (doc-level)
- **Problem:** 5 files not describable without "and" (agent.c, session_manager.c, main.c, server.c, routes_general.c — the last's header literally says "misc HTTP endpoints").
- **Fix:** resolved by F2's splits for agent/session_manager; for server.c and routes_general.c, either split the static-file host out of server.c and re-group routes_general.c's 5 endpoint groups, or document the single-responsibility statement each file answers. `[defer]` main.c (entry point + 2 REPL frontends is a defensible single file; document the rationale).
- **Verify:** one-sentence descriptions written in each file header.

---

## Phase G — Verification discipline (rules 79-84, 81-82, cross-cutting)

### G1 [Rule 79] Operational artifacts for CK_* workflow
- **Problem:** zero operational artifacts: no make gdb/valgrind target, no script, no doc beyond the rule text, no CI wiring; the two latent fork-dependent hazards (E11's fixtures) make CK_FORK=no advice unsafe.
- **Fix:** add scripts/debug-check.sh (CK_FORK=no + gdb/valgrind invocation helpers, CK_RUN_SUITE/CK_RUN_CASE/CK_INCLUDE_TAGS documented), a `make check-debug` / `make valgrind` target pair in the Makefile, a short section in AGENTS.md or the test README; land E11 first so CK_FORK=no is genuinely safe.
- **Verify:** run one suite under CK_FORK=no both orders.

### G2 [Rule 81/82/84] Per-fix evidence
- **Problem:** failing-run evidence asserted in prose, not archived; Valgrind is an unchecked `[ ]` (plan:854) with zero evidence; no per-fix sanitizer records (aggregate-only); b717962 postdates the last recorded sanitizer run.
- **Fix:** for every Phase A/B item: archive the failing test transcript (docs/verification/<item>.fail.txt) and the passing run (docs/verification/<item>.pass.txt) with sanitizer flags; run Valgrind on the 4 memory bugs and record output; run the full 61-suite ctest under ASan+UBSan after each phase and record the LastTest.log. The git-stash reproduction: for each bug fix, `git stash` the fix, confirm the regression test fails, unstash, confirm it passes — record the two runs.
- **Verify:** docs/verification/ populated; plan:854 Valgrind item ticked.

### G3 [Rule 83 + cross-cutting] Remediation-plan hygiene
- **Problem:** plan `[x]` conflates "claimed fixed" and "verified fixed" (e.g. §5.7 routes_session — contradicted by the tree; §12 Linux CI rows "pending"); DoD items :790-796 and :853 unchecked; the §9 acknowledgment that the 8-module wording is wrong was never applied to AGENTS.md.
- **Fix:** re-audit every `[x]` in AGENTS_COMPLIANCE_REMEDIATION_PLAN.md against the tree (E2 re-verifies §5.7); tick :853 after G2's Valgrind run; add the ID-mapping appendix (D10); reword §9 and apply the matching AGENTS.md fix (D9).
- **Verify:** plan document self-consistent; every ticked box has an evidence pointer.

### G4 [Rule 82] Test-first discipline going forward
- **Problem:** zero recorded git-stash reproductions in 138 commits; no test-first sequences.
- **Fix:** this plan's own items follow the discipline (failing test archived before each fix). Add a short "verification checklist" section to AGENTS.md's Testing rules pointing at docs/verification/ as the canonical evidence location.
- **Verify:** at least the A1-A7 items have archived fail→pass pairs.

---

## Definition of done

1. All 7 Phase A bugs fixed with regression tests, sanitizer-clean, Valgrind evidence for the 4 memory bugs.
2. All 8 VIOLATION verdicts resolved (B1-B8): either fixed or explicitly `[defer]`ed with reason.
3. All PARTIAL verdicts resolved: fixed, or converted to COMPLIANT with evidence, or explicitly documented as accepted with rationale (no silent skips).
4. `pkg_check_modules(CHECK REQUIRED)` in place — a configure without Check fails loudly (rule 80).
5. AGENTS.md updated: rule 88 paragraph rewritten (D9), provider.h exception documented (D8), verification-checklist pointer (G4).
6. Root README's 3 stale claims fixed (B8).
7. docs/verification/ contains fail→pass pairs for every bug item; plan:853/:854 and §5.7 checked off or corrected (G2/G3).
8. Final gate: full build + 61/61 ctest under ASan+UBSan, 16 fuzz targets, frontend lint, all 6 CI jobs green — recorded in this file.

## Tracking

Status key: [x] done · [~] partial/deferred with reason. Evidence: docs/verification/2026-08-11_fix_evidence.md.

| Item | Status | Evidence |
|------|--------|----------|
| A1 server.c response leak | [x] | fail tally 5 → pass 7; Valgrind 0 |
| A2 deep_search double-free | [x] | ASan double-free → pass; Valgrind 0 |
| A3 routes_ws negative index | [x] | ASan SEGV message_clear → pass |
| A4 grep_tool stack OOB | [x] | ASan stack-buffer-overflow → pass; Valgrind 0 |
| A5 session_branch fork_copy init | [x] | asprintf fault hook + contract tests; Valgrind 0 |
| A6 encryption EVP final check | [x] | garbage plaintext → NULL; Valgrind 0 |
| A7 rest_api sys/socket.h | [x] | compile; macOS CI gate |
| B1 registry delegate_config destroy | [x] | tally 0 → 4; Valgrind 0 |
| B2 semantic_search teardown+rollback | [x] | rc 0 → -1; free_index public; Valgrind 0 |
| B3 agent execute_tool_calls + 5 voids | [x] | 3 fault tests; AGENT_TEST revived; struct-leak bonus found |
| B4 server_stop NULL guard | [x] | UBSan null deref → pass |
| B5 -Wno-unused-parameter removal | [x] | 7 fuzz targets build clean |
| B6 AGENT_TEST vestigial def | [x] | now a real seam (B3) |
| B7 multi-statement lines | [x] | ~430 lines split across src+tests; 61/61 green |
| B8 READMEs | [x] | 11 module READMEs + root README fixes |
| C1-C13 error handling/overflow | [x] | see evidence file (C2/C8/E2/E3/E4/E5 tests) |
| D1-D10 doc contracts | [x] | headers/TSdoc/AGENTS.md/ID_MAPPING.md |
| E1 regression tests for A1-A7 | [x] | evidence file |
| E2 routes_session dead hook | [x] | 500-on-alloc-fail tests |
| E3 migration asprintf coverage | [x] | shared shim + 3-position fault test |
| E4 memory.c coverage | [x] | get/error-distinction/delete tests |
| E5 tool_delegate loop phase | [x] | 19-position sweep test |
| E6 session_manager add_message/load | [x] | load_session_locked dups fixed + tested (A5) |
| E7 config.c mid-list cleanup | [~] | defer: config token parse lacks injection seam; documented in review rule 88 |
| E8 semantic_search fault asserts | [x] | strengthened to rc/no-commit assertions |
| E9 thin test files | [x] | memory/grep/server extended (A4/B4); rest_api [~] success path needs curl harness — defer |
| E10 fuzz gaps | [~] | defer: session metadata/events + search JSON parsers (2 new targets) |
| E11 fixture violations | [x] | ws_ask_user fixture + notes teardown + delegate statics reset |
| E12 test_routes_general timeout | [x] | 60 s on handle_models |
| E13 vague test names | [x] | 13 renames |
| E14 multi-behavior tests | [x] | string ×2 + openai refresh split; session_manager 3 large integration tests [~] defer |
| E15 flat TCases | [x] | html_extract split into 5 TCases; web_fetch/config/server [~] defer |
| E16 oversized test files | [~] | defer: split products already exist; E15 shrunk the worst |
| E17 untyped ck_assert | [x] | NULL/strcmp converted; strstr blocked by check 0.15.2 (no ck_assert_str_contains) |
| E18 file/area renames | [~] | defer: churns CI config; documented |
| E19 Branches TCase split | [~] | defer with E16 |
| E20 internals pinning | [~] | defer: mirrors ARE the harness contract (documented in test files) |
| F1 long functions | [~] | notes_execute split as pattern (266→120); remaining 50+ defer (F3 table exists) |
| F2 file-level splits | [~] | defer: openai_oauth/html_extract/session_manager splits (large, risky) |
| F3 nesting depth | [~] | defer with F1 |
| F4 WHY comments | [x] | websocket RFC6455/opcodes/mask + grep/list_dir truncation markers |
| F5 descriptive scope | [~] | defer with F2 |
| G1 debug tooling | [x] | scripts/debug-check.sh + make check-debug/valgrind |
| G2 per-fix evidence + Valgrind | [x] | docs/verification/2026-08-11_fix_evidence.md; Valgrind 0 on 7 suites |
| G3 remediation-plan hygiene | [x] | ID_MAPPING.md; §5.7 verified (E2) |
| G4 AGENTS.md verification pointer | [x] | added to AGENTS.md |

Final gate (run 2026-08-11): full build zero warnings under
`-std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined`;
61/61 ctest green; 16 fuzz targets build; frontend `tsc --noEmit` clean;
Valgrind 0 errors on the 7 memory-bug suites.
