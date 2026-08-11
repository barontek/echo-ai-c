# AGENTS.md Compliance Review — Echo AI

**Date:** 2026-08-11
**Repo:** /home/barontek/echo-ai-c (HEAD b717962)
**Method:** 88 delegations (9 batches of parallel review agents), each verifying one rule from AGENTS.md against the codebase. All agents were read-only; no files were modified during the review.
**Scope:** src/ (66 .c, 42 .h), tests/ (61 Check binaries, 845 test cases, 14 fuzz targets), frontend/ (React+TS), build system, CI (.github/workflows/ci.yml), docs.

## Executive summary

| Verdict | Count |
|---|---|
| COMPLIANT | 32 |
| PARTIAL | 48 |
| VIOLATION | 8 |

The codebase is in unusually strong shape: toolchain pinning, include hygiene, sanitizer wiring, kernel-doc contract discipline, and the fault-injection test pattern are exemplary. However, the review found **4 confirmed live memory bugs** (one on the busiest path in the server, one a one-frame heap OOB attack), one stale/overstated claim in AGENTS.md itself about fault-injection coverage, and one build-configuration hazard that can silently produce a build with zero tests.

## Critical findings — confirmed live bugs (at review time)

| # | Bug | Location | Severity |
|---|-----|----------|----------|
| L1 | Response buffer leaked on **every** HTTP response: `server_response` asprintf's `resp`, hands it to `uv_write`; `write_done` frees only `req`, never `buf.base` | src/server/server.c:201-270 (alloc), :158-165 (write_done); contrast websocket.c:70 and server.c:170 which free `req->data` | HIGH — steady-state unbounded growth in the long-running server; present since initial commit |
| L2 | Double-free + use-after-free of `results_json` on non-array search result: item attached at :184 (ownership transferred to `output`), freed by `cJSON_Delete(output)` at :190, then `cJSON_IsArray` reads freed memory and `cJSON_Delete` frees again at :191-192 (guard inverted relative to transfer) | src/tools/deep_search.c:183-192 | HIGH — heap corruption; latent (the 3 bundled providers emit arrays or non-JSON errors), fires the moment any provider returns an object |
| L3 | Heap OOB via negative client-supplied index: `idx = (int)idx_item->valuedouble` unvalidated at :946; `ws_run_fork` clamps `keep = idx + system_prefix` from above only at :387-389, so `idx = -1` yields `keep = -1` → `message_clear(&messages[-1])` (heap underflow read + free of garbage pointers). `(int)1e100` cast is a second vector. Contrast: the regenerate handler (:743) checks `fi >= 0 && fi + system_prefix < count` | src/server/routes/routes_ws.c:946, :383-392 | HIGH — one-frame attack from any connected WS client |
| L4 | Stack OOB in grep tool: `*pos += snprintf(buffer + *pos, cap - *pos, ...)` — snprintf returns the would-be length, so `*pos` can exceed `cap`; the `if (*pos >= cap - 1) break` guard stops only the current file, and the next file's call computes `cap - *pos` as a size_t underflow → write past the 32768-byte stack buffer | src/tools/grep_tool.c:39-41 (search_file), :90 (search_dir) | HIGH — certain with >32 KB of matches across ≥2 files; `-Warray-bounds` cannot see it (runtime index through a pointer parameter) |
| L5 | `message_clear` on uninitialized stack memory: `Message fork_copy;` (no initializer) at :304; `goto cleanup` paths at :307/:317/:322 (OOM in branch_list_ensure/mint) reach `message_clear(&fork_copy)` at :416 before `message_copy` ever zeroes it → free() of garbage pointers | src/session/session_branch.c:304, :307, :317, :322, :415-419 | MEDIUM — UB/crash on OOM paths; exactly the historical bug class AGENTS.md calls out; `-Wuninitialized` cannot see it (cross-function read) |
| L6 | Unchecked `EVP_DecryptFinal_ex` output length: on padding failure `plaintext_len` is never advanced, but `plaintext[plaintext_len] = '\0'` and the buffer's unwritten region is returned/read by callers (session_manager.c:319-330, :654-720, migration.c:246) | src/session/encryption.c:138-152 | MEDIUM — cryptographically gated (requires HMAC-valid token with bad padding) but structurally the classic EVP trap |
| L7 | Missing `<sys/socket.h>` for a real `socket(2)` call (curl open_socket_cb) — macOS `-Wimplicit-function-declaration` under `-Werror` | src/tools/rest_api.c:31 | MEDIUM — the exact macOS-portability trap the rule exists to prevent |

### Fixed during the review window

- **html_extract CP1252 transcode heap overflow** (2× budget vs 3× worst-case write at html_extract.c:1444-1476): fixed in commit b717962 (2026-08-11) with 8 regression tests (tests/utils/test_html_extract.c:356-449) and commit message "fail on old code, pass on new" — the only one of the five audit-discovered bugs that satisfies the regression-test rule.

---

## Rule-by-rule findings

Legend: C = COMPLIANT, P = PARTIAL, V = VIOLATION. All file:line references are to the tree at HEAD b717962.

### Environment and toolchain (rules 1-4)

**Rule 1 — Builds inside `nix develop`; never assume system toolchain. Verdict: C** (2 PARTIAL notes)
- flake.nix:19-40 devShell provides cmake, gnumake, clang, gcc, pkg-config, libuv, curl, sqlite, openssl, cjson, check, valgrind, gcovr, nodejs_22, caddy; flake.nix:10-13 documents lockfile pinning; flake.lock committed (nixpkgs rev 241313f4).
- ci.yml:72-78 backend-nix job wraps configure/build/test in `nix develop --command`.
- PARTIAL 1: ci.yml:33-39 (backend-debian), :55-61 (backend-release), :91-99 (fuzz) run cmake/ctest on a dated Debian container, not inside nix develop — mitigated by `debian:bookworm-20260803-slim` pinning + documented policy (ci.yml:9-19), satisfying the Other/CI clause.
- PARTIAL 2: README.md:10-14 Quick Start shows bare `cmake` commands with no nix-develop note (correct guidance only at README.md:68-69).

**Rule 2 — macOS Homebrew toolchain via Brewfile. Verdict: C**
- Brewfile:4-11 contains exactly the 8 pinned formulae (check, cjson, cmake, curl, libuv, openssl@3, pkgconf, sqlite).
- ci.yml:111 `brew bundle --file=Brewfile`; ci.yml:114 `brew list --versions cmake pkgconf libuv curl openssl@3 sqlite cjson check` records actual versions; ci.yml:127 `ASAN_OPTIONS=detect_leaks=0` set on macOS only, justified at ci.yml:124-125.
- Sanitizer parity structural: CMakeLists.txt:126-127 applies the same flags on both platforms; AppleClang version verification recorded in AGENTS_COMPLIANCE_REMEDIATION_PLAN.md:837 (AppleClang 21.0.0, 41/41).
- Notes: README.md:81 manual-install one-liner uses `pkg-config`/`openssl` instead of `pkgconf`/`openssl@3`; plan doc mixes "41/41" (:837) and "61/61" (:841-843) ctest counts.

**Rule 3 — Other/CI environments pinned and documented. Verdict: C**
- All 6 CI jobs fall into sanctioned categories: backend-debian/backend-release/fuzz pinned via `debian:bookworm-20260803-slim` dated tag (ci.yml:24,46,82); backend-nix via committed flake.lock (ci.yml:69-78); backend-macos via Brewfile (sanctioned float, ci.yml:13-17); frontend via setup-node '22' + committed package-lock.json (ci.yml:136-143). Policy documented at ci.yml:9-19.

**Rule 4 — No silent fallback to a system compiler. Verdict: C**
- Zero `which`/`command -v`/conditional compiler selection anywhere; Makefile:2-3 uses overridable `CMAKE ?= cmake` only; tests/CMakeLists.txt:28-38 is a libFuzzer capability gate (gates targets, never swaps compilers); the only `CC=` assignment is an explicit pin (ci.yml:91 `CC=clang`). All `pkg_check_modules` probes are REQUIRED (CMakeLists.txt:11-17).

### macOS portability (rules 5-7)

**Rule 5 — Every POSIX function's declaring header included explicitly. Verdict: V**
- 35 .c files audited with real POSIX calls; 34 compliant, each carrying its own declaring headers (unistd.h, signal.h, sys/wait.h, fcntl.h, poll.h, dirent.h, sys/stat.h, sys/socket.h, time.h as applicable).
- **VIOLATION: src/tools/rest_api.c:31** — `socket(address->family, address->socktype, address->protocol)` called without `<sys/socket.h>` in the file's include block (src/tools/rest_api.c:8-12). curl's headers may pull it in transitively on Linux — exactly the forbidden pattern.

**Rule 6 — kill/SIGKILL → `<signal.h>` in bash.c, git.c, python_execute.c, server.c, web_fetch.c. Verdict: C**
- All five named files include `<signal.h>` in their own include blocks (bash.c:13, git.c:13, python_execute.c:11, server.c:13, web_fetch.c:13). server.c no longer calls kill(2) — it uses `signal(SIGPIPE, SIG_IGN)` at :748, same header dependency, include present. Full-tree scan: no other file uses kill/SIG* without the include (tests/tools/test_bash.c:6 also compliant).

**Rule 7 — fork/pipe/close/read/write/unlink/access/getcwd/sleep/usleep → `<unistd.h>`. Verdict: C**
- 22/22 files with real calls carry their own explicit `#include <unistd.h>` (12 src files, 10 test files; 191 files audited). All 32 unistd.h includes in the repo are unconditional. The four historically-problematic files (bash.c, git.c, python_execute.c, web_fetch.c) each carry unistd.h + signal.h + sys/wait.h.

### Build flags (rules 8-10)

**Rule 8 — Every C target compiles clean under `-std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g`. Verdict: C**
- Main binary: CMakeLists.txt:6-8 (`-std=c11`, STANDARD_REQUIRED, EXTENSIONS OFF), :124 (`-Wall -Wextra -Wpedantic -Werror`), :126-127 (sanitizers, gated on ENABLE_SANITIZERS default ON at :97), :106 (`-g`, Debug only).
- Tests: tests/CMakeLists.txt:1-5 directory-scoped, inherited by all 61 binaries and all 16 fuzz targets.
- Verified in build/compile_commands.json (gcc 15.2.0) and build-clang/ (clang 21.1.8): flags verbatim on every command.
- Notes: main binary lacks `-g` in Release; scripts/decrypt_session.c and scripts/test_ollama.c build with bare gcc, outside the CMake system.

**Rule 9 — Warnings get `// TODO(reason):` + tracked issue, never blanket pragma. Verdict: C**
- Zero `#pragma` (GCC/clang diagnostic or otherwise), zero `_Pragma`, zero `-Wno-*` flags, zero TODO/FIXME/HACK/TBD comments in src/ or tests/. No warning is silenced anywhere; nothing deferred.

**Rule 10a — Release adds `-O2 -D_FORTIFY_SOURCE=2`; sanitizers stay on in CI. Verdict: C**
- CMakeLists.txt:105-120 pairs `-O2` (line 110) with `_FORTIFY_SOURCE=2` (line 116 Apple / 118 Linux) on non-Debug builds, with a comment stating the pairing requirement; ci.yml:54-61 backend-release explicitly passes `-DENABLE_SANITIZERS=ON` and runs ctest.
- Latent caveat: CMakeLists.txt:116 replaces the default `CMAKE_C_FLAGS_RELEASE` on Apple (dropping `-O3`); a hypothetical macOS Release test build would define `_FORTIFY_SOURCE=2` without optimization. Unreachable today (macOS CI is Debug-only).

**Rule 10b — Never disable a warning class to get it compiling. Verdict: V**
- 7 occurrences of `-Wno-unused-parameter` on fuzz targets: tests/CMakeLists.txt:65 (fuzz_ollama_tool_calls), :77 (fuzz_config), :91 (fuzz_session_deserialize), :103 (fuzz_html_extract), :145 (fuzz_duckduckgo_html_extract), :159 (fuzz_fernet_token), :222 (fuzz_ollama_parse_response).
- Three problems: (1) no in-file rationale — the only justification lives in AGENTS_COMPLIANCE_REMEDIATION_PLAN.md:207/:266, not in the build file; (2) the documented claim "harness files only" is false — `target_compile_options` is target-wide, and compile_commands.json shows the flag applied to production sources (src/config/config.c, src/utils/string_utils.c); (3) empirically vestigial — clang -fsyntax-only with `-Wunused-parameter` enabled produces zero diagnostics on every affected TU, so the flag currently masks nothing.

### Memory ownership (rules 11-14)

**Rule 11 — Every allocation has a documented owner; `_alloc`/`_dup`/`_new` naming; state who frees. Verdict: C**
- All 42 headers verified: every public function returning heap memory states who frees (e.g. session_manager.h:57-58 "caller-owned SessionManager (free with session_manager_free())", json.h:30-31 "owned by obj's tree — do not free it separately").
- 12 documented naming exceptions — neutral names with explicit ownership docs: `get_provider` (factory.c:19, provider.h:62), `get_provider_with_auth`, `conf_load`, `memory_list_all`, `messages_to_json_array`, `apply_context_window`, `json_serialize`, `registry_schemas_json`, `registry_invoke_ask_user`, `encryption_encrypt`/`_decrypt`, `ollama_test_parse_response`.
- One doc gap: `openai_compatible_test_parse_response` (openai_compatible.c:230) — public test-only function returning LLMResponse* with no doc comment and no header declaration.
- ~40 static helpers return allocations under neutral names (run_git family in git.c, openai_oauth.c crypto statics, html_extract.c assemble/text_for_llm, etc.) — freed in-file, no leaks found.

**Rule 12 — No implicit ownership transfer through global state; `_destroy` frees owned pointers. Verdict: V**
- All cross-module global pointers are documented borrowed refs (safety_global, session_manager_global, openai_oauth_global, ask_user_cb, g_session_manager) whose allocating owner also frees.
- **Violation 1:** `delegate_config` (src/tools/registry.c:63) owns 4 str_dup'd strings set by `registry_set_delegate_config` (:262-265), freed only on replacement (:273-276) — `registry_destroy()` (:323-340) never frees them. Shutdown leak on every run; registry.h:239-249 documents registry_destroy without mentioning it.
- **Violation 2:** `search_index` (src/tools/semantic_search.c:55) owns 4 allocated arrays/strings (allocated :186-203) with no production destroy function — frees exist only in the test-only `semantic_search_test_reset()` (:70-92, under SEMANTIC_SEARCH_TEST).
- Struct destroy audit (Tool, ToolResult, SessionManager, Session, Message, LLMProvider, SafetyConfig, Conf, Agent, SearchProvider, SessionList, WSChatCtx, Metrics, all tool ctxs): all free their owned members; the two violations above are the exceptions.

**Rule 13 — Every allocation freed on every exit path; goto cleanup preferred. Verdict: V**
- 45+ functions deep-traced across 20+ files; majority are textbook goto-cleanup. Four confirmed defects:
  - **L4 (HIGH):** server.c response leak — see Critical findings.
  - **L1 (HIGH):** deep_search double-free — see Critical findings.
  - **L2 (MED):** session_branch fork_copy uninitialized clear on OOM paths — see Critical findings.
  - **L3 (MED):** agent.c `execute_tool_calls` — `agent_append_message` failure ignored at :174-178, :191-195, :248-261 (message + str_dup'd fields leak on realloc-OOM); `message_create` result dereferenced without NULL check at the same sites (calloc-OOM crash).
- Style notes (no leaks): duplicated inline frees instead of goto cleanup in `migration_check_and_recover` (~10×), `notes_execute` (~8×), `session_save_stmt`, `init_encryption`; unchecked str_dup results in `load_session_locked` (session_manager.c:642-647) and `message_create` (message.c:53-55).

**Rule 14 — Double-free and use-after-free get security-bug audit rigor. Verdict: P**
- ~1,000 free() sites analyzed (custom analyzer + full manual verification of ~100 candidates across all 108 src files): exactly one genuine double-free/UAF defect — deep_search.c:191-192 (latent behind a provider-output invariant; the 3 bundled providers always emit arrays or non-JSON error strings). No regression test covers the non-array path.
- Adjacent: tool_humanizer.c:42-50 NULL-deref on OOM (`strcmp(style, "bullet")` with `style == NULL` after failed str_dup).
- Two UAF regression tests exist and are genuine: test_semantic_search.c:69 (test_sem_exec_uaf), test_tool_delegate.c:185 (test_delegate_uaf_task_str).

### Error handling (rules 15-18)

**Rule 15 — Fallible functions return status, never bare void. Verdict: P**
- ~200 of ~210 void functions either cannot fail (frees, setters, bounded helpers, test hooks), propagate through a correct channel (HTTP/WS handlers via responses/frames; libuv callbacks mandated by the framework), or carry a documented silent-failure contract (registry_set_delegate_config, rate_limiter_record_unlock_failure).
- **5 violations, all in src/agent/agent.c:** `agent_set_model` (:1088, unchecked str_dup silently clears the model on OOM; header says only "Not thread-safe"), `agent_generate_title` (:626), `agent_apply_title` (:610), `agent_perform_summarization` (:678), `inject_system_with_summary` (:349) — all perform SQLite I/O, LLM network calls, or allocations with every failure path a silent bare `return` and no log.
- Notes: `session_manager_free`/`rate_limiter_destroy` ignore sqlite3_close rc (conventional); `session_manager_lock/unlock` ignore pthread returns (conventional).

**Rule 16 — Every libc/syscall/third-party return value checked. Verdict: P**
- HIGH: 0. MEDIUM 4:
  - M1: search_duckduckgo.c:46-49 — unchecked `str_dup` result dereferenced in the HTML-entity decode loop (NULL-deref crash on OOM).
  - M2: replace_in_file.c:120-121 — `fwrite` result never compared to expected length, `fclose` unchecked; disk-full silently truncates the file and the tool reports success (contrast: write_file.c:93-99 does it correctly).
  - M3: rate_limiter.c:107, :117, :131, :147 — 4× `sqlite3_step` results discarded; line 147 is security-relevant: a failed INSERT means a brute-force unlock attempt is never recorded, weakening the unlock throttle silently.
  - M4: session_manager.c:761, :766, :1070, :1077, :1086, :1093, :1179, :1187, :1192, :1202, :1212, :1219, :1226, :1517, :1530, :1539, :1550 — 17× `pthread_mutex_lock/unlock` results ignored (the sibling module openai_oauth.c checks all ~50 of its mutex calls).
- LOW (~30 sites in 15 groups): 4× fread partial-result ignored (ingest_document.c:78, notes.c:230, read_file.c:68, replace_in_file.c:88); ~13 unchecked sqlite3_bind_*; 5× sqlite3_step loop-exit error treated as SQLITE_DONE; ~12 unchecked str_dup into message/context/session fields (NULL-safe downstream); ~40 curl_easy_setopt (cannot fail with valid options); unchecked str_dup in agent.c:82-83/:1092 and main.c:540.

**Rule 17 — No silent failure paths. Verdict: P**
- ~23 confirmed swallow sites; 143 log_error/log_warn sites and 137 tool_result_error + 99 server_response_error sites show the dominant pattern is compliant. Worst findings:
  - **routes_ws.c:536-546 and :1222-1233** — `message_copy()` failure → messages freed, count zeroed, loop continues: the agent runs the conversation with **zero history**, nothing logged, no error frame to the client. :779-795 branch-switch path returns -1 but the caller only handles `swap_rc == 0` (silent empty history after "successful" switch).
  - **replace_in_file.c:120-121** — write failure reported as success (see rule 16 M2).
  - **change_tracker.c:137-140, :172-177** — `ct_undo`/`ct_redo` consume the undo entry regardless of fwrite success; main.c:458-465 reports "Undone (%d bytes restored)." even for a 0-byte restore — the only copy of the previous content is destroyed.
  - **rate_limiter.c:86-88 vs :104,115,128,143,161,172** — the file's own comment promises failures are "loudly logged"; ~10 of ~11 prepare-failure paths return bare with no log (fail-open without logging).
  - **routes_session.c:68-72** — `session_manager_list_sessions()` failure → HTTP 200 OK `{"sessions":[]}` (DB error presented as "no sessions").
  - **notes.c:129-130** — `opendir()` failure → success result "(no notes)"; notes.c:82 mkdir rc ignored.
  - **tool_memory.c:113-115, :155-158** — DB errors reported as "(not found)"/"(no memory stored)" (memory.c returns NULL for both "no row" and SQL error).
  - **server.c:197 (:268), :158-165, :167-170** — uv_write failures swallowed (leaks req + frame copy; doc at :172-174 admits the silent drop); websocket.c:67-72 `(void)status` on failed async write (mitigated by close).
  - **routes_ws.c:233-234** — queued user message dropped on cJSON_Parse failure with no error frame and no log; :273-274 calloc failure → bare return.
  - **routes_ws.c:153-166 / routes_chat.c:135-150** — stream chunk drops are silent on both transports.
  - **agent.c:1019-1021** — message_copy failure mid-save → session persisted with silently truncated history.
  - **bash.c:204-224 / python_execute.c:89-101** — subprocess launch failures rendered as fake "Exit code: -1" / success with empty output.
  - **write_file.c:74** — `ct_snapshot` failure ignored; the edit proceeds permanently un-undoable.
  - **session_manager.c:209-210** — PRAGMA journal_mode/synchronous rc ignored (durability guarantees silently absent).
  - **config.c:109-116, :118, :121-136, :145-151** — parse errors silently drop entries (some documented).
  - **list_dir.c:52-54, grep_tool.c:54-55/:60-61** — truncation without any marker.
  - **tool_ask_user.c:56-60** — `getline` failure (EIO/closed stdin) → success result "(user did not respond)".
  - **tool_delegate.c:310-312** — OOM duplicating a tool message → message dropped with `continue`, no log.
  - **on_connection (server.c:735-749)** — uv_tcp_init/getpeername/read_start/accept returns unchecked, dropped with no log.

**Rule 18 — Errors carry context. Verdict: P**
- Boundary-logging convention verified: ~140 log sites carry operation + key/value context; all 144 tool_result_error calls carry descriptive messages; no empty/generic strings.
- 4 genuine context-loss paths:
  1. **tools/bash.c:31,34,56,64,80,205-219** — subprocess launch failures collapse to `rc == -1` rendered as `"Exit code: -1\n"` with no explanation (contrast python_execute.c:47/:55 which wrap the same failures as "pipe creation failed"/"fork failed").
  2. **session/session_branch.c** — 35 error returns, zero logs; fork/switch failures surface as generic "fork failed" with no server-side context at any layer.
  3. **agent/agent.c:765, :795-799** — OOM paths return NULL bare; surfaced to the user as generic "no response", indistinguishable from an LLM failure, nothing logged.
  4. **server/websocket.c:77-132, :246-248** — frame-level parse failures silently dropped, zero context.
- Secondary: `strerror()` is used nowhere in src/; errno-based failures (migration.c:153/:323, safety.c:573, bash.c) discard the system error text at every boundary.

**Rule 19 — No signed integer overflow relied on for wraparound. Verdict: P**
- Zero deliberate reliance on signed wraparound; all attacker-reachable parse sites (HTTP, WS frames, SSE, JSON ints, base64, file sizes) have explicit guards (e.g. server.c:500 digit-overflow guard, websocket.c:391 SIZE_MAX guard, session_manager.c:1500-1507 validated with regression test test_session_manager.c:683).
- 4 unguarded sites:
  1. agent.c:687 — `int text_len += strlen(...)` accumulation of untrusted message content (UB past ~2 GB; feeds malloc(text_len+1) at :692).
  2. context.c:357 — same `int total_chars += strlen(...)` pattern.
  3. agent.c:690 — `max_context_chars * 2` with a config value ≥ 2^30 (operator-controlled; no range check, unlike the validated sibling at session_manager.c:1500).
  4. config.c:174-176 — `strtol` → `int` cast without range clamping (feeder for the above).
- Informational: int capacity doubling in tool_delegate.c:215/:269/:321, session_manager.c:1107, memory.c:131, ollama.c:102 (overflow requires 2^30+ elements, realloc fails first).

### Structure and headers (rules 24-27)

**Rule 24 — One header per module, no circular includes; tools exception. Verdict: P**
- All 66 .c files (except main.c and the 24 documented tool modules) have matching headers; the 2026-08 sweep additions factory.h and ollama.h confirmed present.
- Include graph is a DAG — no cycles (routes → server → {agent,tools,session,utils,config,safety,change_tracker} → llm → agent/message leaf).
- **One deviation:** src/llm/provider.h is an orphan header (no provider.c) in a non-exception directory — it is a shared LLM-provider vtable contract behaving exactly like the documented tool.h exception, but the exception is not documented for src/llm/ in AGENTS.md.

**Rule 25 — Include guards on every header, consistent style. Verdict: C**
- 42/42 headers use the identical `ECHO_<UPPERCASED_FILENAME>_H` convention; guard is the first preprocessor directive in every file; `#endif` is the last line of every file; zero collisions; zero `#pragma once`; tests/ contains no headers.

**Rule 26 — No function longer than ~60 lines without a strong reason. Verdict: P**
- 118 functions over 60 lines (102 src + 16 tests); 50 justified (parsers, dispatchers, state machines, subprocess lifecycles — per the remediation plan's F3 table), **52 unjustified**. Worst: run_cli (main.c:327, 271 lines), delegate_execute (tool_delegate.c:87, 270), notes_execute (notes.c:100, 204), handle_models (routes_general.c:198, 191), rest_api_execute (rest_api.c:34, 149), load_session_locked (session_manager.c:604, 140), safety_load_from_conf (safety.c:62, 137), deep_search_execute (deep_search.c:72, 127), handle_unlock (routes_auth.c:122, 126), execute_tool_calls (agent.c:156, 116).

**Rule 27 — Public functions carry doc comments. Verdict: C**
- 312/312 public declarations across 42 headers have kernel-doc blocks directly above them (position-verified programmatically). Quality flags deferred to doc-standards rules: semantic_search.h:23-32 duplicate `Return:`; websocket.h:117 and routes.h:60/:74 doc "Return: void" on int functions.

### Code style (rules 28-38)

**Rule 28 — File size: 300-800 comfortable, 1000+ split signal. Verdict: P**
- 6 files >1000: openai_oauth.c (2151, split candidate — mixes embedded HTTP callback server, OAuth state machine, credential vault), session_manager.c (1583, split candidate — store + OAuth storage + embedded fault-injection scaffolding), openai.c (2003, justified), html_extract.c (1524, justified), routes_ws.c (1344, borderline), agent.c (1163, justified). Tests: test_routes_ws.c (2250), test_session_manager.c (1846) at 1000+.

**Rule 29 — Split along responsibility boundaries; one sentence, no "and". Verdict: P**
- 59/64 files describable without "and". 5 flags: agent.c (loop + title gen + summarization + persistence), session_manager.c (CRUD + OAuth storage + import/export/purge/event log), main.c (entry point + two full REPL frontends), server.c (borderline: HTTP core + static file host), routes_general.c (borderline: header literally says "misc HTTP endpoints" — 5 unrelated endpoint groups).

**Rule 30 — Mirror module boundaries; no catch-all utils.c. Verdict: C**
- src/utils/ is 9 single-purpose modules (circuit_breaker, metrics, http_client, callbacks, html_extract, json, string_utils, logging, rate_limiter); string_utils.c contains string operations only. No misc.c/helpers.c/util.c/common.c anywhere. Sole catch-all smell: routes_general.c's "misc" grouping.

**Rule 31 — 15+ functions usually mixes concerns. Verdict: P**
- 16 files have 15+ functions; 15 single-concern. **openai_oauth.c (99 functions) is genuinely mixed**: PKCE/URL/base64 primitives, JWT parsing, credential vault, a full local HTTP callback server (own bind/listen/accept + pthread_create'd listener thread, ~25 functions), device polling, and the public OAuth state machine.

**Rule 32 — Large static-helper piles split off. Verdict: P**
- **Strongest candidate: html_extract.c** — ~36 of 50 statics (tag scanner, frame machinery, writer, assembler) back the single public `html_extract_text_alloc`; 1524 lines.
- Moderate candidates: openai_oauth.c (crypto-primitive cluster + ~12-static callback-server cluster), openai.c (StreamParser cluster), routes_ws.c (~30 statics behind the single `routes_ws_chat_init` entry).
- Compliant: session_manager.c, session_branch.c, agent.c, server.c, websocket.c, ollama.c, openai_compatible.c (statics distributed across several public functions).
- The F3 sweep split 13 oversized functions but performed no file-level splits under this rule.

**Rule 33 — Functions short and do one thing well. Verdict: C**
- 42 functions sampled across 13 files: 38 ONE THING, 4 refactor candidates (ws_do_handshake handshake+app-init, refresh_credentials wait/refresh modes, ws_run_fork duplicated error blocks, delegate_execute size+duplication), **0 MULTI-THING**. All long functions are pipelines/loops with tightly coupled phases.

**Rule 34 — Minimize nesting depth. Verdict: P**
- 13 of 696 functions (1.9%) nest ≥6; max depth 8: delegate_execute (for>for>if>if>else>if>if), ws_handle_session_id (routes_ws.c:499), ws_apply_query_session (routes_ws.c:1165). Also: ws_handle_provider_frame (7), append_title_text/emit_text (7), write_cb (7). Break-up candidates: delegate_execute (triplicated append pattern), the two routes_ws session-loading twins (copy-pasted blocks), session_deserialize_messages (tool_calls loop). Parser/dispatcher shapes (parse, ws_read_cb, handle_models) justified. Over-indentation noted in routes_ws.c (J3 class).

**Rule 35 — One statement per line; one declaration per line. Verdict: V**
- **265 lines with multiple statements across ~66 of 110 files.** Dominant idiom: single-line `if (x) { free(a); free(b); return NULL; }` error handlers. Worst lines: tool_delegate.c:312 (8 statements: 7 frees + continue), registry.c:269 (5 frees + return), main.c:197/:200/:359/:362/:631/:634 (5 each), json.c:25-29 (case-label triples). Per-file: openai_oauth.c 31, main.c 22, session_branch.c 21, html_extract.c 17, openai.c 12.
- 8 comma-chained declarations: encryption.c:70/:141, html_extract.c:441/:683/:795, test_html_extract.c:333, test_callbacks.c:128/:251.

**Rule 36 — Descriptive names for wide scope. Verdict: C**
- All file-scope globals and all public functions descriptive; zero cryptic identifiers. 3 consistency notes: `g_session_manager` (main.c:164 — the only `g_` prefix in the repo; sibling is `session_manager_global` in registry.c), `cb_` prefix means both "circuit breaker" (circuit_breaker.h) and "callback manager" (callbacks.h), `ct_*` abbreviated family vs fully-spelled test hook in the same header.

**Rule 37 — No typedef'ing to hide what things are. Verdict: C**
- Zero pointer typedefs in any form (`typedef struct X *XPtr;`, `typedef char *string_t;`, `typedef void *handle;` — none). 15 function-pointer typedefs keep the `*` visible. 78 anonymous-struct typedefs hide only the `struct` keyword (accepted idiom); 14 `typedef struct X X;` preserve the tag. No `Foo f = malloc(sizeof(Foo))` hazard exists.

**Rule 38 — Comment WHY not WHAT; every non-obvious line gets a why. Verdict: P**
- ~0.3% of ~665 sampled comment lines are WHAT restatements — only 2 pure ones: config.c:74 ("remove trailing newline if present") and agent.c:580 ("strip leading/trailing double-quotes").
- ~12 missing-WHY gaps concentrated in 3 files: websocket.c (RFC 6455 opcodes 0x8/0x9/0xA/0x1/0x2 at :252-280, mask XOR :285-286, magic `+= 18` at :320, `0x81` at :397, extended-length encoding :388-414), server.c (openat/O_NOFOLLOW symlink-safe descent :91-121, path-traversal defense :74-84/:95, 10 MB cap :356), bash.c (process-group SIGTERM→100ms grace→SIGKILL escalation :125-169, magic 50ms/:166), plus encryption.c key-half split (:184-185/:200-201) and openai.c:177-179 exact-fill bound. ~150 WHY blocks present elsewhere (audit tags C10/B9/D4/J5 etc.).

### Documentation standards (rules 41-56)

**Rule 41 — No comments that restate the signature. Verdict: C**
- Zero lazy patterns across all headers and .c: no `@param x: x`, no `@return: the return value`, no `/* get/set the X */`, no bare field-name comments. All 15 single-line comments above functions add contract info (e.g. session_manager.c:603 "Caller MUST hold sm->lock", context.c:267 "1 token ~ 4 chars"). Weakest in repo: context.c:215 "/* build result */" (borderline WHAT, not a signature restatement).

**Rule 42 — Boring is good / consistency / clarity. Verdict: C**
- Zero cleverness patterns in 26.8k lines: no `*++p`, no `!!x`, no comma operators, no XOR swaps, no nested ternaries, no side-effect returns, no empty loop bodies. Bit-twiddling only where protocols require it (base64/UTF-8/WS framing), written plainly.
- Brace style 100% uniform (next-line `{` in 66/66 files); bare `if (x) return;` guards dominate (658 vs 2 braced); `str_dup` helper used 300+ times, raw `strdup` zero; single goto-cleanup per function is the norm; uniform snake_case; log_error signature uniform. The one unusual construct (session_manager.c:1420 empty-update for-loop) is a documented, correct detach-while-iterating loop.

**Rule 43 — kernel-doc style for public functions. Verdict: C**
- 315 doc comments (312 function + 3 typedef) in 43 headers: 100% open with `name - summary`; 311/312 carry a `Return:` section; all params use `@name:`; blank line before Return in 311/311; `/** */` style uniform. 5 defects: semantic_search.h:23-34 duplicate contradictory `Return:`; session_manager.h:369-380 migration_change_password return values in prose (no tag); http_client.h:55 two params on one line; routes.h:60-63 and :74-77 "Return: void" on int declarations.

**Rule 44 — Headers carry the contract. Verdict: C**
- ~310/310 sampled docblocks carry full contracts — typically 4-5 of the 5 rubric elements (ownership, failure modes, NULL behavior, thread-safety, caveats). Zero MINIMAL/POOR. Tools subsystem is the documented structural exception (contracts live in kernel-doc in each tool .c, e.g. bash.c:243-249). The routes_* headers are contract-rich (every handler documents its HTTP-status error signal, loop-thread requirement, ctx mutation).

**Rule 45 — Implementation carries the why. Verdict: P**
- ~90 WHY comments vs ~11 WHAT labels across 10 sampled files; placement overwhelmingly correct (bug post-mortems C8/C10/J5, Codex #31664 workaround at openai.c:1435-1438, CP1252 rationale at html_extract.c:703-709 live only in .c). 3 duplicated-contract blocks: session_manager.c:1057-1065 (re-prints session_manager.h:159-170 verbatim), websocket.c:191-194 (restates websocket.h:133-145), server.c:174-178 (restates routes.h:65-77 failure semantics; keeps a legitimate stack-buffer why). openai_oauth.c is under-commented on WHY (3 WHY in 2151 lines — single-flight refresh wait loop, callback header anti-rebinding validation, staged token parse all unexplained). Internal duplication: routes_ws.c:647-658 (identical comment pasted back-to-back).

**Rule 46 — File-level header at top of every .c/.h. Verdict: C** (for src/)
- 108/108 src files carry `/* <basename> - <description> */ + "Depends on:" line` in consistent format (the only deviation: utils/logging.h:2 folds Depends-on inline).
- Caveat: tests/ 61 of 83 files lack headers (10 fuzz targets have none at all; 9 have purpose comments); scripts/decrypt_session.c and scripts/test_ollama.c have none. A strict repo-wide reading downgrades to PARTIAL.

**Rule 47 — Ownership documented (who allocates, who frees). Verdict: C**
- ~115 ownership-relevant functions across 30+ headers: ~94 PRECISE (name the free function/destroy/borrower), 21 ADEQUATE (input-only args missing an explicit "borrowed" marker — cosmetic), **0 MISSING**. Exemplars: json.h:30-31, message.h:66-72 (transfer), context.h:39-44 (dual-path ownership), provider.h:16-19 (vtable contract).

**Rule 48 — Lifetime documented. Verdict: C**
- Zero public borrowed/invalidatable pointers lack a validity statement. Invalidation triggers named (registry.h:199-200 "become invalid on the next registry_set_delegate_config() or registry_destroy()"), sqlite memory never escapes (all column pointers copied before finalize), no borrowed getters into session/agent message arrays. Doc/reality checked clean (apply_context_window, trim_messages_by_tokens_new implementations match headers).

**Rule 49 — NULL behavior documented. Verdict: P**
- 234/245 functions fully stated. **HIGH: `server_stop`** (server.h:93) documents "@ctx: ... NULL is accepted" but server.c:779-782 dereferences `ctx->loop` unconditionally — the only "NULL is a no-op" claim not backed by a guard, and the exact class the rule exists to prevent.
- 4 MED return-contract misdocs: ws_add_message_to_json (routes.h:60 "Return: void" on int), server_sse_write (routes.h:74 "Return: void; failures are logged nowhere" — actually returns -1 and DOES log at server.c:185/:192), ws_start_ping_timer (websocket.h:117 "Return: void; never fails" — returns -1 on uv_timer_init failure), semantic_search_index_document (duplicate contradictory Return blocks).
- 5 MISSING (minor): agent_run_new user_input NULL semantics undocumented, route_match method/path, server_sse_write data. 4 PARTIAL: session_deserialize_metadata/_events self-contradictory "must be non-NULL" vs "on a NULL @json_str... untouched", openai_oauth account_id out-param wording.

**Rule 50 — Thread-safety documented. Verdict: P**
- Exemplary for ~95%: session_manager.h lock-holder requirements (:388-390, :414-416), routes_ws.h loop-thread (:28-29/:43-44), per-provider concurrency (openai.h:37-38), caller-serialization honesty (metrics.h:45-46, rate_limiter.h:52-54), logging global level (logging.h:43-45).
- 3 gaps: (1) provider.h:16-19 vtable comment never states whether chat/chat_streaming/extract_structured are concurrent-safe (only per-implementation docs; ollama's is IMPLIED via its `_Atomic` call_seq); (2) middleware.h:65/:80/:92 "Never fails; thread-safe" overstates — they read/write unsynchronized ServerContext state and a RateLimiter that needs caller serialization (safe only under the loop-thread model, which the header doesn't reference); (3) agent.h:209-210 agent_cancel documented "Safe to call from another thread" but implemented as plain `volatile int` (benign in practice, technically UB). Undocumented statics: server.c:370 req_counter, routes_ws.c:1071 approval_counter.

**Rule 51 — Error signaling documented and consistent. Verdict: P**
- 306/310 functions state their convention; 0 IMPLIED; one uniform 0/-1 (+1/0 predicate) convention repo-wide with every deviation documented (delete_session 1/0/-1, purge count-or--1, enum channels OPENAI_MODELS_* 0/-1/-2, OpenAIOAuthTokenResult 0..-4). errno never used as a channel (consistent, correctly undocumented).
- 4 CONFLICTING (same as rule 49's return misdocs): routes.h:60, routes.h:74, websocket.h:117, semantic_search.h:23-34. All four implementations are correct; the docs are wrong.

**Rule 52 — TSDoc for exported TS functions/types/components. Verdict: P**
- 26 of 29 exported items in frontend/src carry proper TSDoc (hooks: useChat.ts:4-16 textbook; components 10/10; ApiClient methods 20/20; types 9/9).
- 4 gaps, all in src/context/ChatContext.ts: `ConnectionStatus` (:4), `EffortOption` (:15), `ChatContext` (:80) — no comment; `EFFORT_OPTIONS_BY_PROVIDER` (:9) — plain `/* */` comment that hover tooltips won't parse. Minor: `api` export (:434) documented via class-level doc.

**Rule 53 — TSDoc 5 non-negotiables (nullability, async failure, effect ownership, mutation, server/client). Verdict: P**
- The critical docs are done right: useChat (nothing to clean up stated), ChatProvider (full AbortController/WS/visibilitychange cleanup contract, :138-155), Sidebar (poll abort on unmount), parseThinkBlocks (purity + never-throws), ApiClient (failure convention + sentinel + browser-only state).
- **One factually wrong doc: ChatInput.tsx:19 claims "owns no effects" but contains a `useEffect` (lines 35-40, textarea auto-resize)** — the stale-docs class the rule warns about.
- #2 (async failure mode) is the most common omission at component level (App, ChatProvider loadData, Sidebar/SetupScreen error mapping in code not docs). #5 (server/client) stated only once (ApiClient class doc). Lint: eslint-plugin-jsdoc @ 'warn' with publicOnly (presence-only; content not machine-enforced; eslint-plugin-tsdoc not installed).

**Rule 54 — Document the interface, not the implementation. Verdict: P**
- 81% of sampled comments pure behavior; 3 clear mechanics-leakers: migration.h:43-49 (migration_change_password: full step-by-step walkthrough of marker file/salt.old/verifier.new/transaction ordering — changes on any reimplementation while the 0/-1/-2 contract doesn't), migration.h:22-29 (internal decision procedure), context.h:37-40 (names internal callees smart_select_alloc/trim_messages_by_tokens_new). 8 mild (session_manager.h "load, append, save triad", http_client.h growth strategy). .c inline comments are uniformly why-level, no contract leakage.

**Rule 55 — One README per module; docs updated in the same commit. Verdict: V**
- **0 of 9 C subsystems have a module README** (src/, src/agent, src/llm, src/session, src/tools, src/server, src/utils, src/safety/config/change_tracker, tests/, deploy/ all missing).
- Root README.md is **stale 3×**: :168-171 references a `web/` directory that hasn't existed since commit e4073c2 (frontend/ migration — README was touched after, in 8591fc7, without updating); :3 claims a "vanilla JS frontend" (actually React+Vite+TS); :94 marks `--chat` "not yet implemented" (implemented at main.c:167/:731/:779). All 16 documented API endpoints verified existing.
- frontend/README.md and scripts/README.md are build docs (setup/scripts/endpoints) with zero design rationale.

**Rule 56 — One comment format everywhere; C touched by non-C tooling extra-explicit. Verdict: C**
- C: one kernel-doc format across all headers + .c tool factories (315+ blocks, 0 deviations; only variance is the Return continuation separator — "Return: void." 40× vs "Return: void;" 58×, "Return: nothing." 16×; 2 `Returns:` in .c comments at session.c:298/session_manager.c:1058). TS: one TSDoc convention, zero mixed jsdoc tags; enforced at warn by eslint-plugin-jsdoc.
- Non-C tooling: ownership/lifetime explicitly spelled out in the 5 contract headers a reviewer/binding would consume — provider.h:16-19, message.h:66-72, session_manager.h:57-58, registry.h:67, tool.h:32-40. Scripts (decrypt_session.c, test_ollama.c) have essentially no doc comments — noted as a gap.

### Testing (rules 57-88)

**Rule 57 — Every new function with a non-trivial contract gets a Check unit test. Verdict: C**
- 61 Check binaries / 845 START_TEST / 14 fuzz targets; all registered 1:1 with add_test; all modules covered (52 dedicated files + 7 verified indirect + 1 exempt main.c). Every named 2026-sweep function maps to test coverage (session_manager_switch_branch, migration_perform_change, semantic_search_index_document, delegate_execute, all F5 status conversions, all F3 split helpers).
- 2 documented caveats: search_duckduckgo.c is fuzz-only (no Check test; creation covered via test_search_provider.c); agent.c title-generation functions have no direct test (provider-dependent, stubbed by design).

**Rule 58 — Every bug fix includes a regression test that would have caught it. Verdict: P**
- ~30 documented fixes have real, registered, bug-path-exercising regression tests (fault-injection hooks genuine and used; UAF tests present; html_extract transcode fixed today with 8 failing-then-passing tests).
- Gaps: (a) plan §5.7 claims `[x]` for a routes_session alloc-fail test that does not exist (hook present, never invoked); (b) fixes B8, C3, C7, C10/C11, D2, H2 shipped without regression tests; (c) **4 of the 5 bugs discovered in this review have no regression tests, and 3 remain live** (server.c response leak, deep_search double-free, grep_tool OOB, routes_ws negative index — only html_extract's transcode bug was fixed, with tests).

**Rule 59 — Fuzz targets for external-input parsers. Verdict: P**
- 16 libFuzzer targets, all compiling the REAL src code via *_TEST hooks (no mirrors — B1/B2 replacement verified); CI fuzz job (ci.yml:80-100) builds all 16 and runs bounded `-runs=1000 -max_len=4096`.
- All named high-risk parsers covered (HTTP parse, WS frames, SSE/buffered responses for all 3 providers, OAuth callback/token/JWT, session deserialize, Fernet token, config, HTML, DDG HTML).
- 3 gaps: session_deserialize_metadata and session_deserialize_events (session.c:318/:354), search_brave.c and search_tavily.c JSON response parsers (unit-tested but not fuzzed).

**Rule 60 — Sanitizer-clean Check runs are a merge requirement. Verdict: C**
- All 5 C-relevant CI jobs sanitizer-ON (backend-debian Debug default, backend-release explicit `-DENABLE_SANITIZERS=ON` at ci.yml:55, backend-nix Debug, backend-macos + sanctioned `ASAN_OPTIONS=detect_leaks=0` at ci.yml:127, fuzz with hardcoded `-fsanitize=fuzzer,address,undefined`). Flags reach all test binaries (tests/CMakeLists.txt:1-5; compile_commands.json 66/66 gcc + 89/89 clang entries). Recorded local run: build/Testing/Temporary/LastTest.log 61/61 clean under ASan/UBSan (gcc 15.2.0, ENABLE_SANITIZERS=ON). Requirement documented in AGENTS.md:187, :26, :232 + plan + CI comments.
- 2 soft spots: Debug jobs rely on the ENABLE_SANITIZERS default (a flip would silently de-sanitize); `make coverage` (sanitizers off, Makefile:73) is documented only in the plan, not flagged dev-only.

**Rule 61 — Test files mirror the source tree. Verdict: P**
- Convention `test_<module>.c` used 100% (zero `*_test.c`); fuzz convention `fuzz_<module>.c` uniform; zero orphan test files; zero uncovered modules.
- 7 modules lack dedicated files (covered indirectly): session.c, session_branch.c, encryption.c, migration.c (all inside test_session_manager.c), tool.c (tools exception), search_duckduckgo.c (via test_search_provider.c + fuzz), opencode_zen.c (via test_factory.c). Name mismatch: test_string.c vs string_utils.c. Area mismatches: test_change_tracker.c (tests/session vs src/change_tracker), test_config.c (tests/utils vs src/config), test_safety.c (tests/agent vs src/safety), routes flattening (tests/routes vs src/server/routes).

**Rule 62 — Don't let one test file cover multiple unrelated sources. Verdict: C**
- All composite files are justified integration coverage of closely related modules within one subsystem: test_session_manager.c (session facade + branch/migration/encryption through the SessionManager API), test_message.c (one session.c deserialize regression — the C1 leak class on Message arrays), test_agent_save.c (agent + persistence), test_routes_ws.c (routes_ws + config as input dependency). No "just because it's convenient" mixing. Arguable split candidate: the 13-test Branches TCase (~600 lines) covering distinct file session_branch.c.

**Rule 63 — `test_<function>_<scenario>` naming pattern. Verdict: P**
- 92.5% conforming; tcase grouping by function exemplary in ~57/61 files. 7 vague names: test_ws_add_message_to_json_basic (test_routes.c:79), test_chat_success_basic (test_routes_chat.c:293), test_on_chunk_basic (test_routes_ws.c:497), test_send_done_basic (:581), test_on_message_message_simple (:1587), test_ws_init_basic (:2002), test_metrics_empty (test_metrics.c:7). ~56 behavior-only names (test_html_extract.c ~26, test_middleware.c ~19, test_circuit_breaker.c 5, test_web_fetch.c ~4) — descriptive but no function component. Flat-tcase exceptions: test_html_extract.c (one tcase, 31 tests), test_web_fetch.c (one tcase, 17).

**Rule 64 — Test file size ~800-1000 split signal. Verdict: P**
- 4 files exceed 1000: test_routes_ws.c (2250, single module — split by feature possible), test_session_manager.c (1846, multi-module — clearest case), test_routes_session_handlers.c (1144, already a split product), test_routes_general.c (1107). 4 in the 800-1000 approaching band (routes_auth 894, ollama 881, openai 840, safety 817) — justified single-module. Split convention is known and exercised (routes_session, ollama_tool_calls, openai_oauth, memory/change_tracker splits exist).

**Rule 65 — One test function tests one behavior. Verdict: P**
- 39/44 sampled single-behavior; all 7 mega-tests (20+ asserts) are single-claim with high verification density. **5 violations**: test_string.c:22 (str_starts_with AND str_ends_with), test_session_manager.c:55 (roundtrip AND delete), :264 (migration AND crash recovery — 20 asserts, two setups), :719 (load AND delete AND save NULL-id — three functions), test_openai.c:670 (refresh success AND failure paths).

**Rule 66 — Many small focused tests over few large ones. Verdict: P**
- No mega-tests (largest single test 106 lines, single-claim); 842 tests median ~25 lines; big files concentrate on error/fault-injection paths (test_session_manager 22/36 error-path, test_routes_ws 35/89). 26/61 files trip the >30 lines/case flag, mostly scaffolding-inflated (stubs/fixtures), 4 genuinely heavy (session_manager 51.3, message 36.3, routes_general 30.8, openai 29.0).
- Inverse failure: 4 thin files under-cover real modules: test_memory.c (1 test for an 8-function module), test_grep_tool.c (1 test, symlink-only), test_server.c (4 tests for 782 lines; security-only), test_rest_api.c (2 tests, no success path).

**Rule 67 — No order dependence; setup and cleanup per test. Verdict: P**
- Clean under default fork mode (all suites `srunner_run_all` in forked children). 6 findings:
  1. **test_routes_ws.c:2170-2178 — `ws_ask_user_cb` TCase is the only one of 13 without a checked fixture.** `stub_uv_now_calls` (declared :59, reset only in setup() at :105) is never reset for these 6 tests; under serial mode the timeout test computes a stale deadline and hangs until tcase_set_timeout(60).
  2. **test_tool_delegate.c:34-39** — scripted mock counters/mode never reset; `test_delegate_executes_tool_calls_before_continuing` (:163-181) asserts exact counts that only hold with pristine statics; mode=1 leaks into the FaultInjection TCase under CK_FORK=no.
  3. test_notes.c:35-37 — teardown is empty; the 2 fault-injection tests leak mkdtemp dirs; HOME never restored.
  4. test_logging.c — no fixtures; inline close(STDERR_FILENO)/dup2 juggling with self-restore only (a mid-test failure leaves fd 2 redirected in serial mode).
  5. test_list_dir.c:24 — creates /tmp/echo_ld_file_probe, never unlinks.
  6. test_safety.c:429-449 — fixed path /tmp/echo_test_ws fixture (not mkdtemp); a prior crash leaves residue that cascades.
- Verified clean: no CWD/umask/locale/signal/stdin dependence (test_git.c chdir is fixture-scoped); no cross-test path collisions; no suite-level shared state (zero suite_setup anywhere).

**Rule 68 — No shared mutable global state between tests. Verdict: P**
- ~98% of mutable test globals reset by checked fixtures or at-test-start self-reset. **2 latent violations** (masked by fork mode): test_tool_delegate.c scripted statics (exact-count assertions, no reset mechanism) and test_routes_ws.c ws_ask_user_cb (fixture gap; the ask_user timeout test depends on default stub_uv_now_calls == 0). All fault-injection counters follow the sanctioned set/reset pattern (e.g. metrics_test_set_alloc_fail(-1) in test body).

**Rule 69 — Deterministic tests. Verdict: C**
- Only 2 real timing primitives in the whole tree, both acceptable: test_bash.c:66-67 bounded poll loop (20 × 50ms = 1s cap, fails fast) and test_bash.c:81 deliberate timeout-feature test (robust category assertion). Clock injection is the norm: test_routes_ws.c stub_uv_now (scripted monotonic clock), test_circuit_breaker.c:51 clock rewind, uv_timer/uv_run stubs. rand() lives only in src (session.c:32, session_branch.c:67) — tests never assert on minted values. Sleep-removal discipline documented and held (test_session_manager.c:1366-1369, :1657-1688 "Deliberately NO sleep here"). The one /proc use is `#ifdef __linux__`-guarded with a portable kill(pid,0) fallback.

**Rule 70 — Cover normal, boundary, and error paths. Verdict: P**
- 17/20 sampled modules FULL (three-way balance + allocation-failure injection; fault hooks genuinely invoked in 17/18 hooked units). NULL-arg coverage exceptional (middleware 11, routes_ws ~15, safety, message); zero-length strong (string, metrics, config, ollama, routes_ws).
- 5 gaps: test_memory.c (1 test: only memory_list_all alloc-fail; no memory_set/get/delete SQLite-error or NULL coverage), test_grep_tool.c (1 test: no normal case, no OOM, no boundary), test_server.c (route dispatch/accept errors stubbed out, untestable), **dead hook routes_session_test_set_alloc_fail** (exists at routes_session.c:25, wired at tests/routes/CMakeLists.txt:24, invoked nowhere), minor: no explicit NULL tests in test_semantic_search.c / test_config.c.

**Rule 71 — Test behavior and output, not implementation details. Verdict: P**
- ~305 tests sampled: dominant patterns are emitted frames, request/response bodies, rendered output, wire bytes, persisted state, return codes — behavior. Stub-call-count assertions correctly judged contract verification (the WS layer's contract is to invoke the agent with the resolved config).
- ~22 tests (~7%) pin internals: **test_change_tracker.c 5/5** (undo_count/redo_count fields — undocumented representation state in the public header), **test_routes_ws.c ~15** (mirrored WSChatCtx struct layout at :20-35 with "matches routes_ws.c" comment; queue node internals :805-853; internal flags), test_tool_delegate.c:173-176 (pins exact chat/tool call sequence), test_ollama.c:394-495 (mirrored WriteBuf struct, file warns "must stay in sync"). Allocation-order fault-injection positions (message.c:245 fail_at 1..17, etc.) are the sanctioned high-fragility class per AGENTS.md's own fault-injection section.

**Rule 72 — Test comments: why, not what. Verdict: C**
- ~115 WHY vs ~5 borderline WHAT across 4,450+ sampled lines (23:1). Dominant pattern is bug-ID regression blocks (A1, A4-A7, B1-B3, B6, B9-B10, C1, C4+C5, C8, C13, C15 — 28 annotations tree-wide), each naming the old behavior, the fix, and which assertion fails on old code. Zero line-by-line "// check the return" restatements. Worst borderline: test_session_manager.c:1565 "/* switch to the old chain */" (pure restatement).

**Rule 73 — Regression tests noted in comment or name. Verdict: P**
- 38 of 39 regression tests carry an explicit regression comment (97%); 32 also self-describing names. **Single gap: `test_ws_init_with_session_id_query` (test_routes_ws.c:2040)** — no comment, scenario-only name; a reader cannot tell it guards the D5-connected ?session_id= resume feature.

**Rule 74 — One TCase per module/behavior area, never a flat suite. Verdict: P**
- 32 files ORGANIZED (multi-TCase by function/behavior), 25 SINGLE-JUSTIFIED (single tool surface), **4 FLAT-VIOLATIONS**: test_html_extract.c (one tcase "html_extract" holding 31 tests across ~6 areas), test_web_fetch.c (one tcase "challenge_fallback" holding 17 tests across 4 areas), test_config.c ("Core" mixing load/get_int/provider_tokens with OOM tests outside the repo's FaultInjection convention), test_server.c ("Parsing" mixing HTTP parser + static-path security). No structural problems (zero flat suite_add_case, zero zero-tcase suites).

**Rule 75 — Test names state the claim being falsified. Verdict: P**
- 476/845 (56.3%) claim-stating; 363 (43.0%) BORDERLINE (scenario named, outcome not: `test_handle_session_get_not_found` vs ideal `_returns_404` — concentrated in the route-handler files and test_safety.c); **6 VAGUE** (operation only): test_cb_create_destroy (test_callbacks.c:105), test_cb_register_and_count (:114), test_json_add_int (test_json.c:88), test_json_add_double (:97), test_json_serialize_object (:107), test_rl_create_destroy (test_rate_limiter.c:6). Exemplars: test_handle_unlock_already_unlocked_rejects_wrong_password, test_parse_strdup_failure_leaves_entry_uncommitted.

**Rule 76 — ck_assert_* typed macros, never bare assert(). Verdict: P**
- **Zero bare assert()** in all of tests/ (including stubs and fuzz) and zero in src/ — the literal rule is fully met; 3,100 ck_assert-family assertions (2,769 typed, 89.3%).
- 331 untyped `ck_assert(cond)` (10.7%): 260 strstr-based substring checks (would print far better diagnostics as ck_assert_str_contains — worst concentration test_routes_ws.c with 106), 30 NULL checks with typed equivalents available (24 in provider-interface tests, e.g. test_factory.c:16-19 `p->chat != NULL`), 23 int/pointer/double comparisons (test_safety.c:20 `ck_assert(cfg->max_file_size == 10485760)`), 3 strcmp/strncmp. 15 boolean predicates have no typed equivalent (acceptable).

**Rule 77 — Fixtures via tcase_add_checked_fixture. Verdict: P**
- 47 checked-fixture registrations across 13 files, all with correct `static void` signatures; **zero unchecked fixtures** (which would be the worse bug — unchecked doesn't run in the forked child). ~27 files self-reset in-test (acceptable in practice, letter deviation); ~19 have no shared state.
- **2 violations** (unreset state): test_tool_delegate.c scripted statics (no fixture, no reset; alloc-fail hooks also never restored to -1) and test_routes_ws.c ws_ask_user_cb TCase (missing fixture; stub_uv_now_calls reset only in the setup() those tests never call). Minor residue: test_write_file.c leaks 6 mkdtemp dirs; test_ingest_document.c leaves its workspace dir.

**Rule 78 — tcase_set_timeout on anything that can hang. Verdict: P**
- 81 timeouts across 42 of 61 files; 69 of 150 TCases untimed; values 5-180s, no timeout-0. All genuinely hang-capable classes covered: real subprocesses (bash 10s, git 60s, python 60s, web_fetch 30s), real sqlite (session_manager 120-180s), pthread (oauth lifecycle 5s), and the ws_ask_user_cb hang risk (60s, line 2177). All untimed TCases assessed pure/in-memory.
- **1 violation: tests/routes/test_routes_general.c** — the handle_models TCase runs `system("rm -rf ...")` subprocesses (:835, :952, :989) plus real temp-file I/O with no timeout (all 9 TCases untimed).

### Check framework / verification discipline (rules 79-88)

**Rule 79 — CK_FORK=no debugging guidance; CK_RUN_* isolation. Verdict: P**
- The rule's mechanics are genuinely implementable: against the pinned libcheck 0.15.2 source, `srunner_create` defaults to `CK_FORK_GETENV`, so all 61 binaries (55× CK_NORMAL, 5× CK_ENV, 1× CK_VERBOSE) honor `CK_FORK=no` and `CK_RUN_SUITE`/`CK_RUN_CASE`/`CK_INCLUDE_TAGS` regardless of print mode.
- But: zero operational artifacts — no make gdb/valgrind target, no script, no doc beyond the rule text itself, no CI wiring; and the two latent fork-dependent hazards (test_tool_delegate.c statics, ws_ask_user_cb fixture gap) mean an agent following the CK_FORK=no advice can hit order-dependent failures.

**Rule 80 — CMake + pkg_check_modules(CHECK REQUIRED check); add_test per binary. Verdict: P**
- add_test registration 1:1 verified — 61 binaries / 61 add_test entries, confirmed in generated CTestTestfile.cmake across all 7 areas; zero GLOB-based discovery; fuzz targets under HAS_LIBFUZZER guard; `make test` wired to ctest.
- **Deviation: CMakeLists.txt:142 is `pkg_check_modules(CHECK IMPORTED_TARGET check)` — NOT REQUIRED**, and the `if(CHECK_FOUND)` gate at :143-146 means a missing Check silently produces a build with **no tests at all**, no configure error. CI could pass running zero tests.

**Rule 81 — Show the failing test before the fix and passing after; clean sanitizer run for memory bugs. Verdict: P**
- Demonstrably practiced: fix commits pair regression tests with fixes (c131291, b717962, 830e05c, 1409dfc "6 new Check tests fail on the old handler... 40 suites green under ASan+UBSan", 81aa2d0, 4e38b3e "Linux LSan caught both leaks"); one physical fail→pass artifact pair on disk (build/Testing/Temporary/LastTestsFailed.log "5:test_html_extract" 00:12 → LastTest.log 61/61 00:15); 61/61 ASan/UBSan run verified genuine (libasan.so.8 linked).
- Shortfalls: failing-run evidence is asserted in prose, not archived (no transcripts, no quoted Check output); the strongest physical evidence is gitignored ephemera; **Valgrind is an explicitly unchecked `[ ]` item in the plan's definition of done (plan:854) — zero valgrind evidence anywhere**.

**Rule 82 — git stash + rerun to confirm reproduction on old code. Verdict: P**
- **Zero recorded instances of the technique** in any doc; the only mention is BRANCHING_IMPLEMENTATION_PLAN.md:223 (a plan item never executed — its fix 3af9f68 landed fix-only with no test). 138 commits contain zero test-first sequences (no "add regression test" before "fix").
- Retroactive verifiability: exactly one regression test can be proven fail-on-old (test_safety_load_from_conf_blocked_paths from c131291, uses only pre-existing APIs); hook-dependent fault tests **cannot** run against parent commits by construction (hooks added in the same commit as the fix — a literal git stash reverts the hooks too); b717962's UTF-8 tests reconstructable but not recorded; fd35ed2's "signal 6" claims bundled in one unverifiable mega-commit. The rule's own discipline ("don't accept claims at face value") is unmet for these claims.

**Rule 83 — Audit findings verified via code-path tracing with line numbers. Verdict: C**
- The plan declares the method explicitly (plan:4-7: "Every finding below was verified by code-path tracing with line numbers (not taken at face value)... demonstrated failing-before / passing-after"), and resolved items embed traces (e.g. §5.1 "message_copy (src/agent/message.c:41-100 — 11 str_dup + 1 calloc)"). **5/5 spot-checked fixes verified present at claimed locations** with matching regression tests: C13 encrypt-fail abort (session_manager.c:808-862 + test :980), B6 list truncation (session_list_append_row :1158-1166 + test :827), C8 import TOCTOU (SM_INSERT_IF_ABSENT :888-896 + test :877), J5 reconnect-wipe (ws_apply_query_session :1207-1235 + test :2040), html_extract transcode (b717962 + 8 tests).
- Caveats: no second-reviewer mechanism anywhere (rule permits self-verification); the plan's `[x]` conflates "claimed fixed" and "verified fixed" (some acceptance criteria ticked while §12 lists Linux "CI pending"); the C/B/J fix IDs live in a deleted audit doc (CHAT_DB_DISCREPANCIES.md, git-recoverable) not the plan.

**Rule 84 — No fix is done until run under sanitizers at least once. Verdict: P**
- Mechanism real and evidenced: sanitizers default ON for every build incl. Release (CMakeLists.txt:97, :126-127); CI runs ctest under ASan/UBSan in 4 jobs + 16 fuzz targets per push; clean 61/61 run recorded with sanitizer-linked binaries; plan records aggregate passes (41/41, 43/43, 60, 61/61 Debug+Release).
- Shortfalls: (1) verification is aggregate-only — no per-fix sanitizer records anywhere in the plan; (2) the 3 live bugs sit on untested paths that sanitizer runs, fuzz (16 targets, none covering them), and coverage (stale Jul 29 gcovr artifacts, un-gated, sanitizer-off by design, no CI job) would all miss — no net exists for untested-path bugs; (3) the final tree commit (b717962, 00:19) postdates the last recorded sanitizer run (00:15) — its coverage rests on an unverifiable CI promise; (4) plan's Linux matrix rows remain "CI pending" and Definition-of-Done item :853 unchecked.

**Rule 85 — Fault-injection pattern (fail-at-N + counter + #define under guard; assert error, no partial commit, reset, re-verify). Verdict: C**
- 30 src files carry *_TEST guards; 20 participate in allocator fault-injection. 26 hook functions (18 set_alloc_fail, 3 set_realloc_fail, 1 set_bind_fail, 1 set_encrypt_fail, 1 set_oauth_alloc_fail, 1 set_rename_fail, 1 test_reset) — **25 invoked by tests, 1 dead** (routes_session_test_set_alloc_fail). Pattern implemented exactly as specified in all 18 requested modules plus session_branch.c (shares session_manager's counter) and migration.c (rename injection).
- Reset+normal-op re-verification verified line-by-line in exemplars: test_metrics.c (fail(2)→-1 + empty render, reset→inc succeeds), test_memory.c, test_change_tracker.c (fail(1)→-1, undo_count unchanged, reset→swap works), test_session_manager.c:572 (bind fail→-1, reset→row intact with original title), test_notes.c (dedicated reset test), test_message.c (fail positions 1-17 with ck_assert_msg per position on all dst fields), test_ollama_tool_calls.c.

**Rule 86 — Test targets opt in via compile definition; production never sees the fake allocator. Verdict: C**
- All ~30 *_TEST macros defined exclusively via `target_compile_definitions(<test_target> PRIVATE ...)` in tests/*/CMakeLists.txt — zero in root CMakeLists (which defines only ECHO_AI_VERSION and _FORTIFY_SOURCE=2), zero in CI/Makefile/flake.
- compile_commands.json proof (all 3 build dirs): all 66 echo-ai entries carry only -DECHO_AI_VERSION; every *_TEST occurrence maps to CMakeFiles/test_*.dir or fuzz_*.dir objects. Reverse-safety verified: all guards wrap purely additive scaffolding (fake allocators, counters, exposed statics, link stubs); no guard alters production logic; no shadow implementations of src functions remain in tests/fuzz/ (only minimal link stubs for external deps). One vestigial dead definition: AGENT_TEST (tests/agent/CMakeLists.txt:49, referenced nowhere).

**Rule 87 — Mock techniques for hard dependencies (forward-declare + #define; test-only init guard). Verdict: C**
- **Exemplar 1 verified:** tool_delegate.c:66-70 forward-declares `td_test_get_provider_with_auth` and `#define get_provider_with_auth td_test_get_provider_with_auth` under TOOL_DELEGATE_TEST; the test-side mock (test_tool_delegate.c:118-132) sets only `chat` + `destroy` on a calloc'd LLMProvider — verified minimal (delegate_execute calls only those two; chat_streaming/extract_structured/ctx never dereferenced). One letter deviation: the #define lives in the guarded source file rather than the test TU — same mechanism, repo convention.
- **Exemplar 2 verified:** registry.c:102-104 `#ifndef REGISTRY_TEST` compiles out the 22-factory registry_init wiring (comment at :102-103 explains the test-binary linking reason); registry.h:98-99 documents it.
- Full inventory: 9 #define substitutions (get_provider_with_auth, uv_write, str_dup/realloc/calloc shims in 6 modules, WS_STATIC linkage flip), same-TU curl redefinition (ollama.c:745-821 under OLLAMA_TEST with capture + scripted failures), link-level stubs in test files (test_openai_oauth.c:15-62 stubs 6 session_manager functions; test_routes_ws.c:195-329 stubs uv + full agent API; test_websocket.c:32-77), 6 shared stub files (tests/{routes,llm}/openai_oauth_stub.c, tests/agent/provider_auth_stub.c, tests/fuzz/*_stubs.c — fuzz stubs document "Minimal link stubs... only so the linker can resolve the rest of the TU"), ~15 init/visibility guards (routes.c ROUTES_TEST skips 5 handler TUs, server.c SERVER_TEST exposes parse_http etc.). One nit: test_openai_oauth.c over-stubs 3 session_manager functions openai_oauth.c never calls (cosmetic).

**Rule 88 — The 8 named modules' allocation-safety paths are covered; new modules get the same treatment. Verdict: P**
- **The AGENTS.md "Known gaps as of this sweep" claim is materially overstated as of HEAD b717962.**
  - Solidly covered (3/8): metrics.c (both multi-alloc commit functions fault-tested), change_tracker.c (snapshot + undo), registry.c (fail positions 1-3 of set_delegate_config, prior config intact asserted).
  - Overstated (5/8): **memory.c** (only memory_list_all's str_dup mid-fail is tested; `memory_get_dup`'s allocation has zero fault coverage and memory_get appears in no test file — its OOM-NULL is indistinguishable from "not found"); **tool_delegate.c** (only entry-phase positions 4-7 + one grow-realloc; the loop-phase commit sites — tool-not-found 5 str_dups at :251-255→:282, tool-result 4-5 str_dups at :296-304→:335, final_content :192, assistant grow :216 — untested); **session_manager.c** (add_message's realloc+rollback at :1246-1290 and load_session_locked's unchecked str_dups :642-645/:658 untested); **config.c** (mid-list token cleanup failure untested; continuation realloc/asprintf/array calloc not injectable); **semantic_search.c** (2 of 3 fault tests assert nothing — no return-value or no-commit assertion; document still committed with missing term on add_term failure).
  - Newer modules: 9/11 properly treated (deep_search, notes, tool_memory, message, context, html_extract, routes_ws, routes_chat, ollama all have working hooks+tests). **Exceptions: routes_session.c (dead hook, zero fault tests — the plan's own §5.7 `[x]` is contradicted by the tree) and migration.c (no allocation-fault hook at all; its 5× asprintf multi-alloc in migration_change_password is untested for OOM).**
  - The remediation plan itself (§9, unchecked items :790-796) acknowledges "the paragraph currently implies the 8-module list is the complete allocation-safety surface; it is not — fix the wording" — AGENTS.md was never updated.

---

## Cross-cutting observations

1. **Two documents, two ID schemes:** fix IDs referenced in code/tests (A1-A13, B1-B10, C1-C15, D1-D5, E1-E2, F1-F6, G, H2, I2-I3, J1-J5) come from CHAT_DB_DISCREPANCIES.md (deleted from the tree in c2c56a8, git-recoverable), NOT from AGENTS_COMPLIANCE_REMEDIATION_PLAN.md (which reuses letters with different numbers: A1-A5, B1-B4, C1, D1-D2, E1-E13, F1-F9). A reviewer matching IDs across docs will be misled.
2. **The repo's own remediation plan is not yet complete by its own definition of done:** Linux CI matrix rows "pending", Valgrind spot-check `[ ]`, Definition-of-Done items at plan:790-796 and :853 unchecked.
3. **Consistent blind spots across tools:** the 3 live bugs all sit in paths no test exercises — sanitizer runs, fuzz targets, and coverage (stale, un-gated, sanitizer-off) would all miss them.
4. **What the repo does best (evidence of discipline):** committed flake.lock + dated CI pins; 100% uniform include-guard and kernel-doc conventions; 312/312 documented public functions with named free functions; 25/26 live fault-injection hooks with reset-and-reverify tests; 61/61 registered ctest binaries; regression tests with bug-ID annotations in 38/39 cases.
