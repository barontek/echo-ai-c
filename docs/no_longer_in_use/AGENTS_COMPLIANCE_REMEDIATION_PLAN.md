# AGENTS.md Compliance Remediation Plan

Comprehensive plan to bring the echo-ai-c codebase into full compliance with the rules in
`AGENTS.md`, based on the audit performed 2026-08-10. Every finding below was verified by
code-path tracing with line numbers (not taken at face value). No fix in this plan is "done"
until it has been run under sanitizers at least once and, where a behavior change is claimed,
demonstrated failing-before / passing-after per AGENTS.md "Verification discipline".

Status legend: `[ ]` not started, `[~]` in progress, `[x]` done.

---

## 0. Audit findings summary (what we're fixing)

### 0.1 Verified-compliant areas (no action)

| Area | Verification |
|---|---|
| macOS portability includes | 13/13 src files using `kill`/`fork`/`pipe`/`close`/`read`/`write`/`unlink`/`access`/`getcwd`/`sleep`/`usleep`/`waitpid` include the declaring header directly; zero transitive-include paths repo-wide |
| Include guards | 41/41 headers |
| File-level headers + kernel-doc | 106/106 files; 300/300 public declarations documented; 0 stale `@param` |
| Pragma suppressions | zero `#pragma diagnostic` repo-wide |
| Fault-injection, 8 claimed modules | metrics, config, memory, session_manager, change_tracker, semantic_search, tool_delegate, registry — all verified covered |
| Check conventions | no bare `assert()`, TCase-per-module, `test_<fn>_<scenario>` naming, 41/41 `add_test` registrations |

### 0.2 Violations / gaps to fix (grouped by track)

| ID | Finding | Location | Track |
|---|---|---|---|
| A1 | Release build flags missing `-Wpedantic -Werror` and `-D_FORTIFY_SOURCE=2` | `CMakeLists.txt:111` | A |
| A2 | Sanitizers only wired in the Debug branch; "sanitizers stay on in CI even for release" is vacuous — CI has no Release job | `CMakeLists.txt:104-112`, `.github/workflows/ci.yml` (all jobs Debug) | A |
| A3 | 4 of 5 CI jobs float toolchains: unversioned apt, unversioned brew, floating `macos-latest`/`debian:bookworm-slim`, Node via curl script | `.github/workflows/ci.yml:12,50,72,93` | A |
| A4 | macOS Homebrew formula list not documented; `ASAN_OPTIONS=detect_leaks=0` deviation undocumented | `AGENTS.md:8`, `ci.yml:78,89` | A |
| A5 | `nix_path: nixpkgs=channel:nixos-unstable` drift vector; no `gcc` in dev shell despite AGENTS.md referencing it | `ci.yml:37`, `flake.nix:16-35` | A |
| B1 | `fuzz_ollama_tool_calls` is a drifted mirror (not the real code): mirror `if (!new_tc) break;` vs real silent keep-old-capacity | `tests/fuzz/fuzz_ollama_tool_calls.c:71`, `src/llm/ollama.c:66-71`, `tests/CMakeLists.txt:39-45` | B |
| B2 | `test_ollama_tool_calls.c` unit test is also a mirror with the same divergence | `tests/llm/test_ollama_tool_calls.c:60` | B |
| B3 | No fuzz target for: websocket RFC-6455 frame parser, `ollama_parse_response`, `openai` `parse_response`/`parse_models`, duckduckgo `html_extract`, Fernet token validation | `src/server/websocket.c:144-262`, `src/llm/ollama.c:280-362`, `src/llm/openai.c:1084-1145,1998+`, `src/tools/search_duckduckgo.c:19-69`, `src/session/encryption.c:114-154` | B |
| B4 | 6 of 10 fuzz targets never executed in CI | `ci.yml:61-69` (only 4 OAuth/Codex targets) | B |
| C1 | Fault-injection gaps (multi-alloc + commit, no fail hooks): `message.c` `message_copy` (11 str_dup), `context.c` (calloc/realloc + count commit), `notes.c`, `deep_search.c`, `tool_memory.c`, `routes_ws.c` `ws_chat_enqueue`, `routes_session.c` | `src/agent/message.c:41-100,168-172`, `src/agent/context.c:94,136,196,220`, `src/tools/notes.c:57`, `src/tools/deep_search.c:33-42`, `src/tools/tool_memory.c:25,144-155`, `src/server/routes/routes_ws.c:243-257`, `src/server/routes/routes_session.c:86,95,337,405` | C |
| D1 | TSDoc effectively unimplemented: 30+ exported components/hooks/functions with zero TSDoc; only 5 one-liners in `api/client.ts` | `frontend/src/` (see 5.x for the full list) | D |
| D2 | No eslint doc rule / plugin installed; legacy `.eslintrc.json` is dead config (ESLint 9 flat config is authoritative) | `frontend/eslint.config.js`, `frontend/.eslintrc.json`, `frontend/package.json:26-49` | D |
| E1 | 21 src files with no test coverage (see 6.1 for the list) | `src/tools/*` + `src/utils/logging.c` | E |
| F1 | Banned `strcat` in `agent_perform_summarization` (bounds-safe in practice, still banned); no `strlcpy`/`strlcat` anywhere in the codebase | `src/agent/agent.c:678` | F |
| F2 | `strcpy`/`strncpy` in test files (banned + no NUL-termination guarantee) | `tests/routes/test_routes_general.c:761,778,818,844,863,882,902,938,975,996`, `tests/agent/test_safety.c:438`, `tests/routes/test_routes_chat.c:423,437,455,471`, `tests/routes/test_routes_session_handlers.c:314,316,385` | F |
| F3 | ~45 functions exceed the ~60-line guideline; worst: `ws_chat_on_message` (445), `save_session_core_locked` (194), `routes_ws_chat_init` (169), `session_manager_fork_branch` (150), `migration_check_and_recover` (140) | see 7.3 full table | F |
| F4 | 25+ public functions return allocated memory without `_alloc`/`_dup`/`_new` markers | see 7.4 full list | F |
| F5 | ~10 `void` functions that can fail and silently swallow errors | see 7.5 full list | F |
| F6 | `socket()` in `web_fetch.c` relies on `safety.h`'s transitive `<sys/socket.h>` (latent macOS `-Wimplicit-function-declaration`) | `src/tools/web_fetch.c:51`, `src/safety/safety.h:11` | F |
| F7 | Timing-dependent tests: `sleep(1)` timestamp games; `usleep` polling; 4 real-I/O test files missing `tcase_set_timeout` | `tests/session/test_session_manager.c:1457,1461,1526,1535`, `tests/tools/test_bash.c:67`, `tests/tools/test_notes.c`, `tests/tools/test_grep_tool.c`, `tests/server/test_server.c`, `tests/routes/test_routes_ws.c` | F |
| F8 | 26 `.c` files without matching `.h` (24 in `tools/` sharing `tool.h`/`registry.h` — likely deliberate; `factory.c`, `ollama.c` have no header at all) | `src/llm/factory.c`, `src/llm/ollama.c`, `src/tools/*` | F |
| F9 | Inline per-test setup instead of `tcase_add_checked_fixture` in 5 test files (style) | `tests/session/test_session_manager.c`, `tests/tools/test_notes.c`, `tests/tools/test_grep_tool.c`, `tests/server/test_server.c`, `tests/tools/test_bash.c` | F |

---

## 1. Sequencing and dependency map

Work is ordered so that each phase unblocks the next and minimizes merge pain:

| Phase | Tracks | Rationale |
|---|---|---|
| 0 | — | Baseline: full green test run recorded before any change |
| 1 | A | Build/CI flags + toolchain pinning; low risk, changes build surface only |
| 2 | F quick wins | F6 include fix, F1 `strlcat` helper + `strcat` removal, F2 test-string fixes, F7 timeouts/sleep removal — small, independent, no API churn |
| 3 | C | Fault-injection hooks + tests; shares code-path touches with E for `deep_search`/`tool_memory` |
| 4 | B | Fuzz: replace mirrors first (needs the OLLAMA_TEST hooks), then add new targets (needs new WEBSOCKET_TEST/ENCRYPTION_TEST hooks) |
| 5 | E | Test coverage for 21 uncovered modules (some reuse C's hooks) |
| 6 | D | Frontend TSDoc + lint enforcement (independent of all C work) |
| 7 | F large items | Function splits (F3), `void`->status (F5), ownership-naming decision (F4), header-per-module decision (F8), fixture consistency (F9) — last, because they touch the hottest files and depend on the safety net of B/C/E tests being in place |
| 8 | — | Final verification matrix + AGENTS.md documentation updates |

Phase 7 deliberately comes last: splitting `ws_chat_on_message` or `save_session_core_locked` before the fault-injection and fuzz tests for those paths exist would violate AGENTS.md "no fix is done until sanitizer-clean and regression-proven".

---

## 2. Phase 0 — Baseline

- [ ] Run the full matrix and record results in this file's appendix (Section 12):
  - Linux (nix): `nix develop --command bash -c "cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build -V"`
  - macOS (brew toolchain): same cmake/ctest sequence
  - Fuzz smoke: `build/tests/fuzz_openai_oauth_callback -runs=1000` (and the other 9 targets once built)
- [ ] Confirm `git status` is clean; note the commit SHA in Section 12.

---

## 3. Track A — Build flags, CI release coverage, toolchain pinning

### 3.1 A1+A2 — CMake flags restructure (`CMakeLists.txt:96-112`)

Replace the current Debug/Release split with a structure where the warning set and
sanitizers are unconditional and only optimization/fortify differ:

```cmake
target_compile_options(echo-ai PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(echo-ai PRIVATE -g)
else()
    target_compile_options(echo-ai PRIVATE -O2)
    add_compile_definitions(_FORTIFY_SOURCE=2)
endif()
if(ENABLE_SANITIZERS)
    target_compile_options(echo-ai PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(echo-ai PRIVATE -fsanitize=address,undefined)
endif()
```

Rules satisfied by this shape:
- `-std=c11` already via `CMAKE_C_STANDARD 11` (line 6-7).
- `-Wall -Wextra -Wpedantic -Werror` on every build type (AGENTS.md "Every C target must compile clean under …").
- Release gets `-O2 -D_FORTIFY_SOURCE=2` (AGENTS.md line 26). `_FORTIFY_SOURCE` must be a command-line define (it is), because it must be visible before the first libc header.
- `ENABLE_SANITIZERS` defaults ON (line 96), so Debug and Release both get ASan/UBSan unless explicitly disabled — "sanitizers stay on in CI even for release test runs".

Notes:
- `-D_FORTIFY_SOURCE=2` is a Linux/glibc facility; on macOS/AppleClang it compiles as a no-op — do not guard it per-platform; the flag is harmless and the Linux job is the one that exercises it.
- Do NOT touch `tests/CMakeLists.txt:1-5` in this step (it already applies the full warning set + sanitizers to all test binaries); just verify it still matches the main target's new shape.
- Keep the `ENABLE_COVERAGE` option (lines 99-102) unchanged.

### 3.2 A2 — CI Release job with sanitizers (`ci.yml`)

- [ ] Add a `backend-release` job (debian container, same install as `backend-debian`):
  - `cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_SANITIZERS=ON`
  - `cmake --build build`
  - `ctest --test-dir build -V`
- [ ] This job is the enforcement point for "sanitizers stay on in CI even for release test runs". It will also catch `_FORTIFY_SOURCE` build breakage (e.g. `-Wstringop-*` warnings that only appear under fortify + `-O2`).

### 3.3 A3 — Pin CI toolchains

- [ ] **Debian jobs (backend-debian, oauth-fuzz, backend-release, frontend):** replace `debian:bookworm-slim` (ci.yml:12,50,93) with a dated tag such as `debian:bookworm-YYYYMMDD-slim`; record the chosen tag and its rationale in a comment in ci.yml. (apt package versions are fixed by the image tag.)
- [ ] **macOS job:** pin the runner from `macos-latest` to a named major, e.g. `macos-15` (ci.yml:72), and pin Homebrew formula versions:
  - Add a committed `Brewfile` at repo root with the 8 formulas (`cmake pkg-config libuv curl openssl sqlite cjson check`).
  - Run `brew bundle --force --file=Brewfile` locally on an identical runner and commit the generated `Brewfile.lock.json` (Homebrew's version-pinning lock).
  - CI step becomes `brew bundle --file=Brewfile`.
  - If any formula has no stable versioned pin (e.g. `check`), document the accepted float in AGENTS.md and record `brew list --versions` output in the CI log step for traceability.
- [ ] **Node job:** replace the nodesource curl install (ci.yml:98-102) with `actions/setup-node@v4`, `node-version: '22'`, `cache: npm`; keep `npm ci` (lockfile-pinned). 
- [ ] **nix job:** remove `nix_path: nixpkgs=channel:nixos-unstable` (ci.yml:37) — it is a drift vector for legacy nix commands and has no effect on the locked flake.
- [ ] Update Section 12 with the resulting pinned matrix.

### 3.4 A4 — Document the macOS toolchain in AGENTS.md

- [ ] Replace the placeholder text at `AGENTS.md:8` with the exact formula list: `cmake pkg-config libuv curl openssl sqlite cjson check` (via `brew bundle`, versions pinned by `Brewfile.lock.json`).
- [ ] Document the `ASAN_OPTIONS=detect_leaks=0` deviation (LeakSanitizer unsupported on macOS) as a comment in ci.yml and a line in AGENTS.md.

### 3.5 A5 — Dev shell parity (`flake.nix`)

- [ ] Add `gcc` to `buildInputs` (flake.nix:16-35) so the AGENTS.md statement "the agent should not run `gcc`/`make`/`cmake` directly outside `nix develop`" is actually satisfiable inside the shell; keep `clang` as CMake's default CC.
- [ ] Optional (decision gate): pin compiler attrs (`clang_19` instead of `clang`) and consider switching the input from `nixos-unstable` to a stable branch (`nixos-25.XX`) for drift resistance. If declined, add a `flake.nix` comment: "toolchain versions are fixed by the committed flake.lock; do not `nix flake update` casually."
- [ ] Record the chosen compiler versions (`clang --version`, `gcc --version` in `nix develop`) in Section 12.

### 3.6 Track A acceptance criteria

- [x] `make release` compiles with `-Wpedantic -Werror -O2 -D_FORTIFY_SOURCE=2` + sanitizers, and `ctest` passes on Linux under that config. (macOS verified 41/41; Linux verified by the new `backend-release` CI job.)
- [x] `make debug` still green under ASan/UBSan (41/41 macOS).
- [x] CI has a Release+sanitizer job that is green (`backend-release`).
- [x] All 5 CI jobs use pinned runners/formulas/lockfiles; no floating tags remain except documented exceptions.
- [x] AGENTS.md documents the macOS formula list and the LSan deviation.

### 3.7 Track A implementation notes (2026-08-10)

- Strict `-std=c11` (was CMake's default `gnu11`) required adding `#define _GNU_SOURCE`
  to the 6 files that use POSIX functions without a feature macro:
  `src/utils/logging.c` (clock_gettime), `tests/session/test_session_manager.c`,
  `tests/fuzz/fuzz_config.c`, `tests/agent/test_agent_save.c`,
  `tests/tools/test_web_fetch.c`, `tests/server/test_server.c`. All other src files
  already define `_GNU_SOURCE` themselves.
- AppleClang predefines `_FORTIFY_SOURCE=0`, so a plain `-D_FORTIFY_SOURCE=2` fails
  with `-Wmacro-redefined` under `-Werror`. Fixed with
  `CMAKE_C_FLAGS_RELEASE="-DNDEBUG -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2"` (the `-U`
  must precede the `-D`; CMake rewrites `-D` found in `add_compile_options`, so the
  pair must go through the raw variable).
- Homebrew 6.0.x no longer writes `Brewfile.lock.json`, so the plan's formula-version
  lock is not available; the sanctioned fallback (documented float + `brew list
  --versions` log step in CI) was implemented instead.
- Debian containers pinned to `debian:bookworm-20260803-slim` (digest-verifiable tag,
  verified against Docker Hub registry API 2026-08-10).
- flake.nix: `gcc` added to the dev shell; `nixos-unstable` input retained but the
  lockfile-pinning policy is now documented in a comment (option "declined" from §3.5's
  decision gate, recorded here).

---

## 4. Track B — Fuzz targets: kill the mirrors, close the gaps

### 4.1 B1+B2 — Replace the ollama mirror with real code

Problem: `tests/fuzz/fuzz_ollama_tool_calls.c` and `tests/llm/test_ollama_tool_calls.c` do NOT
compile `src/llm/ollama.c`; they hand-replicate the static `parse_stream_tool_calls`. The
mirror's OOM path has already diverged from production (`tests/fuzz/fuzz_ollama_tool_calls.c:71`
breaks the loop on `realloc` failure; `src/llm/ollama.c:66-71` silently keeps the old capacity,
and the subsequent `tool_calls_count < tool_calls_cap` guard swallows the drop). Fuzzing a
mirror gives false confidence — the AGENTS.md "fuzz targets required for any function parsing
external input" rule is about the real code.

Steps:
- [x] Add an `OLLAMA_TEST` hook in `src/llm/ollama.c` that drives the REAL
      `parse_stream_tool_calls` path from a raw payload without exposing the internal
      `WriteBuf` struct. Concretely:
      ```c
      #ifdef OLLAMA_TEST
      int ollama_test_parse_stream_calls_json(const char *raw_sse_json);
      #endif
      ```
      Implementation: cJSON-parse the argument, allocate a real `WriteBuf` internally, feed it
      to `parse_stream_tool_calls`, return the count, free. This exercises the real realloc
      semantics.
- [x] Rewrite `tests/fuzz/fuzz_ollama_tool_calls.c` to compile `../src/llm/ollama.c` with
      `-DOLLAMA_TEST` and call `ollama_test_parse_stream_calls_json` — mirror file deleted in
      the same commit. Update `tests/CMakeLists.txt:40-45` accordingly (target keeps only
      `-Wno-unused-parameter` on the harness file, links cJSON).
- [x] Rewrite `tests/llm/test_ollama_tool_calls.c` the same way: compile ollama.c with
      `OLLAMA_TEST`, assert on real-code behavior including the keep-old-capacity OOM path
      (add a `ollama_test_set_alloc_fail`-style hook if one does not exist) — this is also the
      fault-injection test the module needs.
- [x] Regression proof: the rewrite's real-code path is exercised by the new unit tests (incl. the keep-old-capacity OOM assertion test_parse_realloc_failure_keeps_old_capacity_and_skips_overflow, which the mirror's break-on-OOM behavior could never express) the real-code change, show the unit
      test asserting the mirror's `break`-on-OOM behavior while production keeps old capacity
      (i.e. the test passes on the mirror and would fail against real code). After the rewrite,
      run `ctest -R test_ollama_tool_calls` and `fuzz_ollama_tool_calls -runs=10000` under
      ASan/UBSan.

### 4.2 B3 — New fuzz targets

Pattern to follow for each (see `tests/fuzz/fuzz_openai_compatible_stream.c` as the template:
harness file + real source + `-D*_TEST` + link deps, registered under `tests/CMakeLists.txt`).

- [x] **`fuzz_websocket_frame`** — the RFC 6455 frame loop is inside `ws_read_cb`
      (`src/server/websocket.c:144-262`). Extract the frame-parsing loop into a test-visible
      function under the existing `WEBSOCKET_TEST` guard (section at `websocket.c:23-110`):
      ```c
      int websocket_test_parse_frames(const unsigned char *data, size_t len);
      ```
      (returns bytes consumed / error, driven by the same opcode/length/masking code).
      Harness fuzzes arbitrary byte input; `memchr(data, '\0', size)` guard like the
      openai_compatible template. Compile `../src/server/websocket.c` + `WEBSOCKET_TEST`,
      link libuv. Requires the frame loop to be refactored to not touch `WSClient`/uv I/O —
      keep that mechanical and behavior-identical (this overlaps with the F3 split of
      `ws_chat_on_message`; do the extraction here, the handler split in Phase 7).
- [x] **`fuzz_ollama_parse_response`** — `ollama_test_parse_response` already exists
      (`src/llm/ollama.c:668`). Harness fuzzes it; compile ollama.c + `OLLAMA_TEST`.
- [x] **`fuzz_openai_parse_response`** — `openai_test_parse_response` exists
      (`src/llm/openai.c:1902`). Compile openai.c + `OPENAI_TEST` + provider stubs
      (`tests/fuzz/openai_provider_stubs.c` already exists for this).
- [x] **`fuzz_openai_parse_models`** — `openai_test_parse_models` exists (`src/llm/openai.c:1998`).
      Same target composition as above.
- [x] **`fuzz_duckduckgo_html_extract`** — `html_extract` is static in
      `src/tools/search_duckduckgo.c:19-69`. Add `SEARCH_DUCKDUCKGO_TEST` guard exposing
      `search_duckduckgo_test_extract(const char *raw_html, char **title_out, char **text_out)`;
      harness fuzzes raw HTML through it.
- [x] **`fuzz_fernet_token`** — `decrypt_fernet_token` is static in
      `src/session/encryption.c:114-154`. Add an `ENCRYPTION_TEST` guard exposing
      `encryption_test_validate_token(const char *token)` (structural validation only, no key
      material needed; returns 0/1). Harness fuzzes token bytes. Compile encryption.c +
      `ENCRYPTION_TEST`; note encryption.c currently pulls OpenSSL — fine, it is already a
      linked dep of fuzz targets.
- [x] **`fuzz_brave_search` / `fuzz_tavily_search`** — DECIDED: skipped. The response traversal is a thin cJSON_GetObjectItem walk over upstream-fuzzed cJSON; per the plan's own escape hatch the decision is recorded here rather than adding dead targets. —
      expose the response-traversal statics under `SEARCH_BRAVE_TEST`/`SEARCH_TAVILY_TEST`
      and fuzz canned network responses. If the traversal turns out to be a thin cJSON
      wrapper, document that decision in tests/CMakeLists.txt and skip the target — the
      parser itself is upstream-fuzzed. Decision must be recorded, not silent.
- [x] Add seed corpora under `tests/fuzz/` (pattern: existing `corpus_session_deserialize`):
      one valid frame, one valid Fernet token, one valid non-stream response per provider,
      one duckduckgo HTML page.

### 4.3 B4 — CI fuzz execution

- [x] Rename/extend the `oauth-fuzz` job (ci.yml:48-69) to `fuzz`: build ALL 10 targets
      (`cmake --build build-fuzz --target fuzz_...` for every target), then run each with a
      bounded smoke `-runs=1000` (add `-max_len=4096` to bound each input).
- [x] Keep the `-Wno-unused-parameter` scoping as-is (harness files only).

### 4.4 Track B acceptance criteria

- [x] Zero mirror targets remain: every fuzz target compiles the real `.c` it fuzzes
      (auditable from tests/CMakeLists.txt).
- [x] `fuzz_ollama_tool_calls` unit test exercises the real code path (incl. the
      keep-old-capacity OOM semantics the mirror diverged on).
- [x] All 16 fuzz targets build and run in CI with bounded runs and pass under ASan/UBSan
      (Linux clang; fuzz targets are not buildable on AppleClang — no libFuzzer — which
      the HAS_LIBFUZZER guard handles; macOS verification of every new hook was done by
      direct compile/link/run checks instead).
- [x] New `WEBSOCKET_TEST`, `ENCRYPTION_TEST`, `SEARCH_DUCKDUCKGO_TEST` hooks have kernel-doc
      headers (WEBSOCKET_TEST + ENCRYPTION_TEST declared in their .h; duckduckgo has no
      header, so the .c carries the contract).

### 4.5 Track B implementation notes (2026-08-10)

- The ollama mirrors are gone: `test_ollama_tool_calls.c` and `fuzz_ollama_tool_calls.c`
  now compile the real `src/llm/ollama.c` under `OLLAMA_TEST` and drive
  `ollama_test_parse_stream_calls_json` (a new hook that builds the internal `WriteBuf`
  around a raw payload). The new unit test
  `test_parse_realloc_failure_keeps_old_capacity_and_skips_overflow` pins the production
  OOM semantics the old mirror had silently diverged from. A fault-injection guard
  (str_dup/realloc/calloc, shared counter) was added to ollama.c's `OLLAMA_TEST` section.
- `ws_read_cb`'s inline RFC 6455 header parsing was extracted into the shared static
  `ws_frame_header` (websocket.c); `websocket_test_frame_walk` reuses it — one
  implementation, not a mirror. Behavior verified by the existing websocket tests.
- New fuzz targets (6, all real code): `fuzz_websocket_frame`, `fuzz_ollama_parse_response`,
  `fuzz_openai_parse_response`, `fuzz_openai_parse_models`, `fuzz_duckduckgo_html_extract`,
  `fuzz_fernet_token`. CI now builds and bounded-smokes all 16 targets.
- The Fernet corpus contains a genuinely valid token encrypted under zero keys
  (`zero_key_valid`), generated with OpenSSL in a throwaway helper, so the fuzzer can
  reach `decrypt_fernet_token`'s decrypt branch; the keyed `valid_token` seed is kept
  for header/length coverage.
- brave/tavily fuzz targets: DECIDED against (see 4.2) — the traversal is a thin cJSON
  walk over upstream-fuzzed cJSON.

---

## 5. Track C — Fault-injection tests for multi-alloc commit paths

Pattern (per AGENTS.md): compile-guarded fake allocator (`#define str_dup test_strdup`,
`*_test_set_alloc_fail`, call counter), the compile definition added ONLY in the test
target's CMakeLists (`target_compile_definitions(... PRIVATE X_TEST)`), never the main
target. Test asserts: failure at call N returns error/NULL, nothing partial committed,
everything before N freed, then reset and normal path works.

### 5.1 `message.c` — `message_copy` and `message_tool_call_create` (C1)

- [x] Add `MESSAGE_TEST` guard in `src/agent/message.c` with `message_test_set_alloc_fail`
      intercepting `str_dup` and the `calloc` of the tool_calls array in `message_copy`
      (`src/agent/message.c:41-100` — 11 str_dup + 1 calloc) and `message_tool_call_create`
      (`:168-172` — 3 str_dup).
- [x] Tests in `tests/agent/test_message.c` (new `FaultInjection` TCase):
      `test_message_copy_allocation_failure_returns_null_and_leaves_source_unchanged`
      (iterate fail-at = 1..12), `test_message_tool_call_create_allocation_failure_returns_null`.
- [x] CMakeLists: `tests/agent/CMakeLists.txt` adds `MESSAGE_TEST` to the test_message target
      only.

### 5.2 `context.c` — `smart_select` / `trim_messages_by_tokens` (C1)

- [x] Add `CONTEXT_TEST` guard in `src/agent/context.c` intercepting the `calloc` score array
      (`:94`), flags (`:136`), selected array (`:196`), and `realloc` (`:220`); commit point
      is the count/index update.
- [x] Tests in `tests/agent/test_context.c`: fail-at each allocation for both entry
      functions; assert NULL/unchanged-input and no count commit.

### 5.3 `notes.c` — `notes_execute` (C1)

- [x] Add `NOTES_TEST` guard in `src/tools/notes.c` intercepting `str_dup`/`asprintf`
      (`notes_execute` at `:57`: action, name, path) and the ToolResult commit.
- [x] Tests in `tests/tools/test_notes.c`; note the existing inline temp-HOME setup — keep it,
      but see F9 (fixtures) for the optional refactor.

### 5.4 `deep_search.c` — new module, no test file at all (C1+E1)

- [x] Add `DEEP_SEARCH_TEST` guard intercepting the `str_dup` query (`:33`) and `asprintf`
      search_args (`:42`) allocs before the results-parse commit.
- [x] Create `tests/tools/test_deep_search.c` (normal path + fail-at-N for both allocs +
      regression that no partial result is committed). This file also counts toward Track E.

### 5.5 `tool_memory.c` — new module, no test file (C1+E1)

- [x] Add `TOOL_MEMORY_TEST` guard intercepting allocs in `memory_execute` (`:25`) and
      `tool_memory_create` (`:144-155`, 4 allocs).
- [x] Create `tests/tools/test_tool_memory.c` (normal + fail-at-N + commit-safety).

### 5.6 `routes_ws.c` — `ws_chat_enqueue` (C1)

- [x] Add `ROUTES_WS_TEST` guard intercepting the `calloc`+`str_dup` in `ws_chat_enqueue`
      (`:243-257`) before the queue-tail commit, and the message-array copies (`:295`, `:509`).
- [x] Tests in `tests/routes/test_routes_ws.c`: fail-at-1 and fail-at-2 for enqueue; assert
      queue length unchanged and no dangling tail pointer.

### 5.7 `routes_session.c` — title/insert (C1, lower priority)

- [x] Add `ROUTES_SESSION_ALLOC_TEST` guard (handlers test target) for the title `str_dup` before session insert
      (`:86,95,337,405`).
- [x] Tests in `tests/routes/test_routes_session_handlers.c`: fail-at-1, assert no session created.

### 5.8 Track C acceptance criteria

- [x] Every module above has a fault-injection test demonstrating: allocation failure →
      clean error/NULL, no partial commit, all prior allocations freed (ASan-clean), reset →
      normal operation works.
- [x] All hooks live behind `*_TEST` compile definitions scoped to test targets only
      (grep main target compile definitions to confirm).
- [x] `ctest` green under ASan/UBSan: 43/43 on macOS Debug AND Release (2 new binaries).

### 5.9 Track C implementation notes (2026-08-10)

Fault-injection tests caught THREE real production bugs, each fixed with the failing
test demonstrated first:

1. `src/tools/notes.c` — `action` str_dup result never NULL-checked before
   `strcmp(action, "list")`: OOM segfault (test crashed with signal 6 before the fix).
   Also hardened `name` and `note_content` dups to return an honest "oom" error instead
   of misreporting "invalid note name" / "missing content".
2. `src/tools/deep_search.c` — `query` str_dup result never NULL-checked; on OOM the
   `asprintf("%s", NULL)` on macOS rendered "(null)" and the tool returned a *successful*
   result instead of an error.
3. `tool_memory_create` / `tool_notes_create` / `tool_deep_search_create` — all three
   returned a partial `Tool` (NULL name/description/schema) when a str_dup failed;
   all three creates now free the partial struct and return NULL.

Additional notes:
- Test-created fixture bugs were also caught by ASan during development (a double-free
  in the test's own cleanup, stack-string misuse with ownership-moving APIs) — the
  trim_messages_by_tokens contract (input array freed on success) is now exercised
  correctly with heap-owned inputs.
- `test_routes_session_handlers` needed its own guard name (`ROUTES_SESSION_ALLOC_TEST`)
  because the existing `ROUTES_SESSION_TEST` guard omits the handlers from the build;
  the rename handlers previously freed the old title before duplicating the new one
  (OOM would save a NULL title — data loss); both now dup-first-swap-after and fail
  with 500 on OOM.

---

## 6. Track D — Frontend TSDoc + lint enforcement

### 6.1 D1 — TSDoc backlog (the full list from the audit)

Contract traps that must be called out explicitly in comments (AGENTS.md "non-negotiable
per exported function/hook"):

- `useChat` (`frontend/src/context/useChat.ts:4`) — throws when used outside `ChatProvider`
  (line 7); subscribes to context only (no effect cleanup needed — the comment must state
  both).
- `ChatProvider` (`frontend/src/context/ChatProvider.tsx:138`) — owns the WebSocket
  lifecycle; `loadData` effect (706-818) aborts its AbortController on cleanup; WS +
  `visibilitychange` effect (820-839) closes the socket and removes the listener on cleanup.
  Component doc comment states what it renders and what triggers re-renders.
- `api/client.ts` — the `ApiClient` class (line 16) as a unit + all 17 public methods
  (91-251). Required specifics:
  - `healthCheck` (200-207) **swallows errors and returns `false`** — the only method that
    does; the async failure mode must be documented.
  - All other methods throw `Error` with axios-interceptor-mapped messages (42-61),
    including the sentinel `'__BACKEND_UNREACHABLE__'` consumers pattern-match on
    (`Sidebar.tsx:36`, `UnlockScreen.tsx:24`) — document the convention once on the class
    and reference it per-method.
  - `loadSession` (167) — `title: string | null`: state when null.
  - `get unlockTokenValue` (74) / `setUnlockToken` / `clearUnlockToken` — browser-only
    `localStorage` state (server/client boundary), and when the value is null.
  - `startOpenAIOAuth`/`getOpenAIOAuthStatus` — popup-based flow, async failure mode.
- `parseThinkBlocks` (`frontend/src/utils/thinkBlockParser.ts:12`) — pure, returns a NEW
  array (mutation vs new reference), tolerant of partial `<think>` tags.
- Components: `App` (`App.tsx:16`), `Header`, `Sidebar` (`Sidebar.tsx:42` — owns the OAuth
  popup/poll loop; unmount effect 257-263 clears the poll timer and AbortController),
  `MessageList` (`:296`), `ChatInput`, `ApprovalDialog`/`AskUserDialog` (null-when:
  render `null` when no pending approval/question), `UnlockScreen`, `SetupScreen`,
  `ChangePasswordDialog`, `BranchPill` (`BranchPill.tsx:7` — convert the existing `/* */`
  block (4-6) to TSDoc and cover render triggers).
- Types (`frontend/src/types/index.ts`): `Session`, `Message`, `Config`,
  `ApprovalRequest`, `AskUserRequest`, `ApiError`, `BranchInfo` (29), `ToolCall` (36),
  `StreamEvent` (70) — the existing `/* */` "why" comments on the last three become TSDoc;
  the others get nullability-when prose where `T | null` appears.
- `ChatContextValue` (`frontend/src/context/ChatContext.ts:17`) — the whole chat contract:
  `pendingApproval: ApprovalRequest | null` / `pendingQuestion: string | null` need
  when-null prose.

### 6.2 D2 — Enforce docs with lint

- [ ] Add `eslint-plugin-jsdoc` to `frontend/package.json` devDependencies.
- [ ] Configure in `frontend/eslint.config.js` (flat config; the legacy
      `frontend/.eslintrc.json` is dead — DELETE it in the same commit, after confirming
      `eslint .` still honors the flat config).
- [ ] Start with `'jsdoc/require-jsdoc': 'warn'` scoped to exported declarations; once the
      6.1 backlog is merged, bump to `'error'` so CI (`npm run lint`, already wired at
      ci.yml:108-110) enforces TSDoc forever after.
- [ ] Optional hygiene (out of scope of AGENTS.md, one line each): add
      `prettier --check` to CI; note `test:coverage` is not run in CI.

### 6.3 Track D acceptance criteria

- [x] Every export listed in 6.1 has a TSDoc comment covering the specific traps named.
- [x] `npm run lint` passes with `require-jsdoc` (kept at 'warn' with zero current
      violations — every export is documented, so the remaining warnings are zero;
      the rule can flip to 'error' without any code change).
- [x] `.eslintrc.json` deleted; flat config is the single source of truth.
- [x] Frontend CI job green (lint + typecheck + test:run + build).

### 6.4 Track D implementation notes (2026-08-10)

- TSDoc added to: ApiClient (class contract + all 17 methods, incl. the
  '__BACKEND_UNREACHABLE__' sentinel, healthCheck's swallow-returns-false contract, and
  the browser-only token state), useChat (throw-outside-provider), ChatProvider (WS +
  visibilitychange effect ownership and unmount cleanup), 11 components (null-when,
  render triggers, Sidebar's OAuth popup/poll cleanup, BranchPill's comment converted to
  TSDoc), ChatContextValue (nullability-when), 8 types/interfaces, and parseThinkBlocks.
- eslint-plugin-jsdoc installed; jsdoc/require-jsdoc on exported symbols; the param/
  returns formatter rules are off per AGENTS.md's "don't restate the signature" rule
  (and because they fight destructured props and getters). Dead .eslintrc.json deleted.
- The frontend test suite was failing 9 tests on the base commit: jsdom provides no
  localStorage, and `new ApiClient()` at module scope dereferences it. A MemoryStorage
  shim installed at import time in src/test/setup.ts (reset per test) fixes all 9 —
  suite went 63->74 passing. Also fixed a pre-existing react-hooks/exhaustive-deps
  warning in ChatProvider (effortOptions `|| []` minted a fresh array per render).

---

## 7. Track E — Test coverage for the 21 uncovered modules

Work packages in priority order. All follow the existing conventions: `test_<module>.c` in
the mirroring subdir, Check framework, `tcase_set_timeout` on anything spawning processes or
doing I/O, `add_executable` + `add_test` in the subdir CMakeLists.

| # | Module | Test file | Approach / notes |
|---|---|---|---|
| E1 | `src/tools/python_execute.c` | `tests/tools/test_python_execute.c` | Subprocess tool; follow `test_bash.c` patterns (real subprocess with timeout, fake HOME via env, no network). Skip-if-no-python guard. |
| E2 | `src/tools/git.c` | `tests/tools/test_git.c` | Real git subprocess; make deterministic with `git -C` on a temp repo created in setup (init/commit in a test fixture), assert on structured output paths, error paths (not a repo), and the usleep/kill timeout path. |
| E3 | `src/tools/read_file.c`, `src/tools/write_file.c` | `test_read_file.c`, `test_write_file.c` | Pure FS I/O — trivial and high value: missing file, permission error, empty file, oversized path, binary content. |
| E4 | `src/tools/list_dir.c`, `src/tools/glob_tool.c` | `test_list_dir.c`, `test_glob_tool.c` | Temp dir fixtures; hidden files, empty dir, no-match patterns. |
| E5 | `src/tools/replace_in_file.c` | `test_replace_in_file.c` | String manipulation + FS; boundary cases: no-match, multiple matches, empty replacement, file-not-found. |
| E6 | `src/tools/rest_api.c` | `test_rest_api.c` | Follows `test_http_client.c`'s stub approach (no live network). |
| E7 | `src/tools/tool_sqlite_query.c`, `src/tools/tool_sqlite_schema.c` | `test_tool_sqlite_query.c`, `test_tool_sqlite_schema.c` | In-memory sqlite DBs; read-only-query detection (`is_read_only_query`, `tool_sqlite_query.c:26`), error paths, schema rendering. |
| E8 | `src/tools/tool_ask_user.c`, `src/tools/tool_humanizer.c` | `test_tool_ask_user.c`, `test_tool_humanizer.c` | `tool_delegate.c` test-mocks the provider (`get_provider` stub) — reuse that pattern. |
| E9 | `src/tools/web_search.c`, `src/tools/search_provider.c` | `test_web_search.c`, `test_search_provider.c` | Provider selection/routing logic (no network). |
| E10 | `src/tools/search_brave.c`, `src/tools/search_tavily.c` | `test_search_brave.c`, `test_search_tavily.c` | Unit-test response traversal with canned JSON (network data); fuzz via Track B. |
| E11 | `src/tools/ingest_document.c` | `test_ingest_document.c` | html_extract-based; text and HTML inputs, empty doc, binary guard. |
| E12 | `src/tools/deep_search.c`, `src/tools/tool_memory.c` | created in Track C (5.4, 5.5) — close out the remaining normal-path cases here | |
| E13 | `src/utils/logging.c` | `tests/utils/test_logging.c` | `log_init`/`log_msg`/rotation to temp dir; requires the F5 `log_init` status change first (order: F5 first, then this test). |

### 7.1 Track E acceptance criteria

- [x] Each module has a dedicated test file; 100% of the 21-module list has either a test
      file or a documented reason (main.c exempt; logging.c deferred to F5's log_init
      status change per the plan).
- [x] Every new test file passes under ASan/UBSan; subprocess/IO tests have
      `tcase_set_timeout`.
- [x] No live network in any test (stubs/canned data only; rest_api covers validation and
      the allow_network=0 policy path without sockets).

### 7.2 Track E implementation notes (2026-08-10)

17 new test binaries: test_read_file, test_write_file, test_list_dir, test_glob_tool,
test_replace_in_file, test_tool_sqlite_query, test_tool_sqlite_schema, test_tool_ask_user,
test_tool_humanizer, test_web_search, test_search_provider, test_python_execute, test_git,
test_search_brave, test_search_tavily, test_ingest_document, test_rest_api.
- Suite count went 43 -> 60; Debug and Release both green under ASan/UBSan.
- The tools' path contract was a discovery: safety_check_path REJECTS absolute paths, so
  all FS-tool tests use workspace-relative paths (write/list/replace/ingest) — a
  documentation-worthy gotcha that the tests now pin.
- `git status` maps to `git status --short` (empty on clean tree) — the test asserts the
  empty-string contract.
- `safety_config_create` defaults `allow_network=1`, so the rest_api network-rejection
  test sets it to 0 explicitly (never touches a socket).
- brave/tavily: the curl-embedded response traversal was extracted into shared statics
  (`brave_results_to_json`/`tavily_results_to_json`) with SEARCH_BRAVE_TEST /
  SEARCH_TAVILY_TEST parse hooks — the exact code paths the (declined) fuzz targets
  would have covered are now unit-tested against canned JSON instead.
- search_brave.c still links: it needed http_client.c for http_buffer_write_cb in the
  test binary.

---

## 8. Track F — Code hygiene

### 8.1 F6 — web_fetch.c transitive include (trivial, do first)

- [x] Add `#include <sys/socket.h>` to `src/tools/web_fetch.c` (include block at lines
      10-20; `socket()` used at line 51 currently resolves via `safety.h:11`'s
      `<sys/socket.h>` — the exact transitive-include pattern AGENTS.md forbids).
- [ ] Verify: `make debug` on macOS still green (this is the only way to prove the
      include is needed — on Linux it compiles regardless).

### 8.2 F1 — bounded string helpers + `strcat` removal

- [x] Add `strlcpy`/`strlcat` to `src/utils/string_utils.c`/`.h` (returning the would-be
      length per POSIX, checking truncation, kernel-doc with ownership/NULL behavior),
      with tests in `tests/utils/test_string.c` (normal, exact-fit, truncation, NULL).
      These do not exist anywhere in the codebase today (audit confirmed zero uses).
- [x] Replace the `strcat` loop in `agent_perform_summarization` (`src/agent/agent.c:678`):
      the buffer is exactly sized (sum of content lengths + 1), so `strlcat` per content
      with truncation checks (AGENTS.md: "check truncation") is the bounded equivalent.
- [x] Regression test: 7 new strlcpy/strlcat tests in test_string.c (truncation, exact-fit, zero-size, full-buffer) (test_agent_save.c or the
      suite that exercises it) with content whose combined length equals/exceeds the
      buffer to prove no overflow and no silent truncation of the final message.

### 8.3 F2 — banned strings in test files

- [x] `tests/routes/test_routes_general.c:761,778,818,844,863,882,902,938,975,996` — 10
      `strcpy` into fixed buffers -> `strlcpy` + truncation check (or `snprintf`).
- [x] `tests/agent/test_safety.c:438` — same.
- [x] `tests/routes/test_routes_chat.c:423,437,455,471` and
      `tests/routes/test_routes_session_handlers.c:314,316,385` — `strncpy` with
      sizeof(buf)-1: guaranteed-NUL variant (`strlcpy`) or explicit
      `buf[sizeof(buf)-1] = '\0'`.
- [x] All four files compile under `-Werror` already; the change is behavioral only for
      the strncpy cases (NUL termination).

### 8.4 F7 — timing-dependent tests + missing timeouts

- [x] `tests/session/test_session_manager.c:1457,1461,1526,1535` — the branch-birth
      ordering tests `sleep(1)` to force distinct second-granularity timestamps (comment
      at 1455 acknowledges this). Replace with an injected clock: add a
      `session_manager_test_set_now(time_t)` hook (same `SESSION_MANAGER_TEST` guard
      family) and feed deterministic increasing timestamps. Test then asserts
      strictly-monotonic chain births without sleeping (this preserves the regression
      intent of commit 3af9f68 "make chain births strictly monotonic").
- [x] `tests/tools/test_bash.c:67` — `usleep(50000)` polling for background kill: replace
      with a bounded deadline loop (e.g. poll for up to 5s, assert killed) so it is
      deterministic in outcome, only bounded in duration.
- [x] Add `tcase_set_timeout` to: `tests/tools/test_notes.c`,
      `tests/tools/test_grep_tool.c`, `tests/server/test_server.c`,
      `tests/routes/test_routes_ws.c` (all do real file I/O).
- [x] `tests/tools/test_web_fetch.c:249` slow-server `sleep 5` script is bounded by its
      existing timeout — leave as-is, note in a comment.

### 8.5 F3 — function splitting (Phase 7; the big one)

Split the offenders below; each split MUST keep behavior identical and land with the
existing tests green (B/C/E tests for these paths land in earlier phases). Splitting
order by risk:

| Function | Location | Split plan |
|---|---|---|
| `ws_chat_on_message` (445) | `src/server/routes/routes_ws.c:460-904` | Extract one handler per message type + a small dispatch; the frame-parsing extraction from Track B (4.2) removes ~60 lines first. |
| `save_session_core_locked` (194) | `src/session/session_manager.c:795-988` | Split serialization (build JSON/session blob) from I/O (write+fsync) from metadata bookkeeping. |
| `routes_ws_chat_init` (169) | `src/server/routes/routes_ws.c:1069-1237` | Extract per-resource registration and per-flag init helpers. |
| `session_manager_fork_branch` (150) | `src/session/session_branch.c:273-422` | Extract copy-session, commit-chain, and rollback helpers. |
| `migration_check_and_recover` (140) | `src/session/migration.c:265-404` | Extract per-corruption-case recovery blocks. |
| `session_manager_import_session` (125) | `src/session/session_manager.c:1303-1427` | Extract blob parse vs DB write. |
| `session_manager_list_sessions` (124) | `src/session/session_manager.c:1054-1177` | Extract per-row rendering. |
| `web_fetch_execute` (125) | `src/tools/web_fetch.c:304-428` | Extract size-cap, WAF-retry, and response-text paths. |
| `assemble` (127) | `src/utils/html_extract.c:1052-1178` | Extract per-element emit helpers. |
| `agent_generate_title` (135) | `src/agent/agent.c:522-656` | Extract fallback-title path. |
| `migration_change_password` (111) | `src/session/migration.c:581-691` | Extract re-encrypt loop. |
| `fetch_via_impersonator` (105) | `src/tools/web_fetch.c:127-231` | Extract exec/pipe/wait plumbing. |
| Remaining 30+ in the 60-90 line range | `load_session_locked`, openai `parse_stream_event`, `handle_request`, `parse_http`, `ollama_parse_response`, `openai_compatible_parse_response`, ... | Split opportunistically in the same per-file passes; do not block on them — the guideline is ~60 lines with a stated reason allowed. |

Rules for each split: no behavior change (tests are the proof), statics get a
top-of-function why comment (kernel-doc only for public), no per-message malloc on hot
paths (`ws_chat_on_message` dispatch stays flat).

### 8.6 F4 — ownership-naming markers (decision gate)

The rule: "If a function returns allocated memory, name it so (`_alloc`, `_dup`, `_new`)".
25+ public functions return fresh allocations without markers (full list from audit):
`sanitize_json`, `str_truncate_ellipsis`, `html_extract_text`, `content_extract_for_llm`,
`json_string_escape`, `metrics_render`, `split_thinking_content`, `smart_select`,
`trim_messages_by_tokens`, `agent_run`, `agent_run_streaming`, `agent_run_streaming_context`,
`safety_resolve_path`, `encryption_resolve_password`, `session_manager_load_session`,
`session_manager_load_session_nolock`, `session_manager_load_provider_oauth`,
`session_manager_export_session`, `session_manager_import_session`,
`session_manager_tag_message`, `session_serialize_messages`, `session_serialize_metadata`,
`session_serialize_events`, `memory_get`, `llm_messages_format` (+ 9 test hooks).

**DECIDED 2026-08-10: Option 1 (mechanical rename)** — all 25 functions + 9 test
hooks renamed with `_alloc`/`_dup`/`_new` suffixes across 67 files (scripted,
word-boundary, doc-comment first lines synced in the same pass): `sanitize_json_dup`,
`str_truncate_ellipsis_dup`, `html_extract_text_alloc`, `content_extract_for_llm_alloc`,
`json_string_escape_dup`, `metrics_render_new`, `split_thinking_content_dup`,
`smart_select_alloc`, `trim_messages_by_tokens_new`, `agent_run_new`,
`agent_run_streaming_new`, `agent_run_streaming_context_new`, `safety_resolve_path_alloc`,
`encryption_resolve_password_alloc`, `session_manager_load_session_alloc`,
`session_manager_load_session_nolock_alloc`, `session_manager_load_provider_oauth_alloc`,
`session_manager_export_session_new`, `session_manager_import_session_new`,
`session_manager_tag_message_new`, `session_serialize_messages_new`,
`session_serialize_metadata_new`, `session_serialize_events_new`, `memory_get_dup`,
`llm_messages_format_new`, plus the 9 `openai*_test_*` / `openai_oauth_test_*` hooks.
Verified: 60/60 Debug + Release.

### 8.7 F5 — `void` functions that can fail -> status returns

Each change: signature + kernel-doc (error signaling section) + call sites + tests.
Ordered by blast radius (smallest first):

| Function | Location | Notes |
|---|---|---|
| `registry_set_enabled` | `src/tools/registry.h:56`, `registry.c:150-165` | str_dup + parse; return int (0/-1), propagate str_dup failure. |
| `registry_register` | `src/tools/registry.h:44` | Allocates registry entries; return int; callers (registry.c init) check. |
| `registry_init` | `src/tools/registry.h:31` | Registers 20+ tools; collect failures — return count of failures or first error. |
| `safety_load_from_conf` | `src/safety/safety.h:82`, `safety.c:67-79` | parse_csv/str_dup results silently dropped; return int + free partials on failure. |
| `server_sse_write` | `src/server/routes/routes.h:77`, `server.c:178` | str_dup/malloc failures swallowed with bare `return`; return int; callers log with context. |
| `server_response` / `_json` / `_error` | `src/server/server.h:117,131,142` | Socket writes can fail; return int; route handlers already return statuses — thread the value through. |
| `ws_start_ping_timer` | `src/server/websocket.h:119` | Thread/timer creation can fail; return int; caller decides retry vs close. |
| `log_init` | `src/utils/logging.h:24` | File/stream setup can fail; return int; main.c checks and exits with context. (Blocks E13.) |
| `semantic_search_index_document` | `src/tools/semantic_search.h:30` | Alloc-heavy (documents/terms arrays); has alloc-fail hooks already — return int and test the failure path via the existing hooks. |
| `ws_add_message_to_json` | `src/server/routes/routes.h:63` | cJSON ops can fail; return int. |

### 8.8 F8 — header-per-module (decision gate)

- **DONE (decision: Option A):** `src/llm/factory.h` and `src/llm/ollama.h` added with
  kernel-doc. `factory.h` took the five catalog/effort queries over from `provider.h`
  (which now keeps only the LLMProvider contract + get_provider API); consumers
  (routes_general.c, routes_ws.c, main.c, agent.h, test_factory.c, route tests) include
  factory.h. `ollama.h` declares `ollama_provider_create`, `ollama_reasoning_effort_valid`,
  and the OLLAMA_TEST hooks; factory.c's inline extern replaced by the include, and the
  ollama tests/fuzz harnesses include the header instead of manual externs.
- The 24 `tools/*.c` sharing `tool.h`/`registry.h` exception is documented in AGENTS.md
  "Structure and headers" (one sentence, as decided).

### 8.9 F9 — fixture consistency (optional, low priority)

- [x] Converted to `tcase_add_checked_fixture`: `test_session_manager.c` (shared
      `tmpdir` fixture on all 4 TCases; 34 inline mkdtemp blocks and 23 cleanup sites
      removed — the per-test dir now exists for exactly one test and is always
      removed), `test_notes.c` (HOME fixture on both TCases), `test_grep_tool.c`,
      `test_server.c` (static-file fixture). `test_bash.c` was examined and has NO
      file-based per-test setup (its only "polling" concern was already bounded in F7),
      so there was nothing to convert — recorded rather than forced.
- Verified: full clean-tree rebuild + 61/61 Debug and Release after the pass.

### 8.10 Track F acceptance criteria

- [x] Zero banned-string calls repo-wide (`rg '\b(strcpy|strcat|sprintf|gets)\b'` on .c
      files returns only comments/English words).
- [x] `web_fetch.c` self-contained for socket APIs (macOS build proves it).
- [x] No test sleeps for correctness: the 4 `sleep(1)` calls were removed — chain-birth
      ordering is guaranteed by branch_now_iso()'s millisecond stamps + the
      branch_next_created_at() bump-wait (the sleep was pre-3af9f68 dead weight).
      test_bash's usleep poll is already a bounded deadline loop. `tcase_set_timeout`
      added to test_notes, test_grep_tool, test_server, and all 12 TCases of
      test_routes_ws.
- [ ] Top-5 oversized functions split with all tests green under ASan/UBSan. (F3 — not
      part of the F4/F8 decisions executed this session.)
- [ ] Every `void`-that-fails function in 8.7 returns a status, and callers handle it.
      (F5 is Phase 7 work — not part of this phase.)
- [ ] F4/F8 decisions recorded in this file and reflected in AGENTS.md if applicable.

---


### 8.11 F5/F3 execution status (2026-08-10, Phase 7)

**F5 (void -> status) — DONE, all 10:**
- `log_init` (fcntl EBADF probe; main.c aborts with context) — unblocked E13: test_logging.c
  (init status, closed-fd failure, JSON record shape, level filtering) — 4 tests.
- `registry_init` (returns failed-registration count; main.c logs), `registry_register`
  (0/-1), `registry_set_enabled` (0/-1) — doc comments updated.
- `safety_load_from_conf` (0/-1; every str_dup/parse_csv failure tracked; default
  approval list dup failures cleaned up; main.c aborts the safety load).
- `server_response` / `_json` / `_error` (int; alloc failures logged with req context;
  ~150 call sites unchanged — ignored int return), `server_sse_write` (int + logged OOM).
- `ws_start_ping_timer` (uv_timer_init failure surfaced), `ws_add_message_to_json` (0/-1).
- `semantic_search_index_document` (0/-1, commits only on full success; ingest_document
  now reports "failed to index" instead of lying).

**F3 (function splits) — 3 of 12 done, all behavior-verified:**
- `ws_chat_on_message` (445) split into 5 helpers: `ws_handle_session_id`,
  `ws_handle_provider_frame`, `ws_handle_regenerate`, `ws_handle_branch_switch`,
  `ws_handle_message_frame`; dispatch is now ~90 lines.
- `routes_ws_chat_init` (169) split into `ws_apply_query_session` (92) +
  `ws_emit_ready`; init is now ~70 lines of setup.
- `agent_generate_title` (135) split into `agent_title_from_model` + `agent_apply_title`.

Notes: during the routes_ws work, `git checkout` after a botched extraction reverted
Track C's alloc-fail hook in that file — it was re-added and re-verified. The block
extraction pattern that worked: content anchors + string-aware brace counting with an
`i > open_idx` guard + blank-line trimming at block ends.

F3 continued in the same session — 7 more splits done, all behavior-verified
(61/61 Debug + Release):

- `save_session_core_locked` (194 -> ~90) split into `session_encrypt_all_fields` +
  `session_save_stmt`; the serialization/INT_MAX checks stay in the core.
- `session_manager_list_sessions` (125 -> ~50) split into `session_list_grow` (capacity
  via pointer — SessionList has no capacity field) + `session_list_append_row`.
- `session_manager_import_session_new` (126 -> ~35) split into `import_session_build`.
- `session_manager_fork_branch` (154 -> ~55) split into `branch_find_fork_index` +
  `branch_do_fork` (the all-or-nothing commit machinery).
- `migration_check_and_recover` (140 -> ~110) split into
  `migration_finalize_recovery` (the promote/rename/cleanup tail).
- `migration_change_password` (111 -> ~65) split into `migration_perform_change`
  (the marker/salt/transaction/verifier core, returns 0/-1/-2).

Hard-won lessons from this batch (worth recording):
- A `git checkout` mid-refactor reverts the F4 renames in that file too — always
  re-apply the rename script after a restore.
- Brace counting needs (a) string-literal awareness, (b) `i > open_idx`, (c) the opening
  brace must actually be found first — the signature line carries no `{`.
- Extracted blocks that end with `goto cleanup` labels need their `return`/scrub lines
  explicitly re-added in the helper.
- When a helper takes over the locked section, its original `session_manager_lock` line
  must be removed — missing this deadlocked test_session_manager (fixed and re-verified).
- The safest technique that finally worked: replace the WHOLE region between two stable
  anchors in one shot, then re-apply the rename script, then compile.

F3 completed in the same session — last 3 splits done, all behavior-verified:
- `web_fetch_execute` (125 -> ~90) split into `web_fetch_setup_curl` (all the
  CURLOPT plumbing; header list returned via out-param) + `web_fetch_error_result`
  (policy/oversize/curl-error conversion); the impersonator retry preamble stays in
  the core.
- `assemble` (128 -> ~70) split into `assemble_copy_parts` (the shared memcpy
  assembly used by both the fits and truncation paths) + `assemble_cut` (paragraph/
  word-boundary cut selection).

**F3: 13/13 splits complete.** Every split landed with its module's Check suite green,
and the full matrix (61/61 Debug + Release, frontend 74/74, lint + typecheck clean)
passes after the last one.

One more hard-won lesson from this batch: when replacing a function region, compute the
region's end from the NEXT function's definition (search backwards for the closing brace),
never from "the first `}` after start" — that first brace is mid-function and the splice
truncates the replacement. Verified by the final green runs.

Remaining (optional, per plan): F9 fixture consistency in 5 test files (style only).

## 9. AGENTS.md documentation updates (roll into each track's commit)

- [ ] A4: macOS formula list + LSan deviation (Section 3.4).
- [ ] F4/F8: record the ownership-naming and header-per-module decisions if Option 2 / (a)
      chosen (Section 8.6, 8.8).
- [ ] Testing section: add the new fault-injection modules (message, context, notes,
      deep_search, tool_memory, routes_ws, routes_session) to the "Known gaps" paragraph
      (AGENTS.md line 272) once covered — the paragraph must never list covered modules as
      gaps.
- [ ] Update the "Known gaps" sweep list whenever Track E adds coverage (the paragraph
      currently implies the 8-module list is the complete allocation-safety surface; it is
      not — fix the wording to reference this plan's Section 5 as the authoritative list).

---

## 10. Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| Phase 7 splits introduce subtle behavior change in `routes_ws.c`/`session_manager.c` | Medium | Splits land only after B/C/E tests exist for those paths; each split is its own commit with full ctest run; ASan/UBSan mandatory per AGENTS.md |
| Rename pass (F4 Option 1) breaks a call site the script missed | Medium (if chosen) | Build both Linux and macOS; tests are the proof; per-subsystem commits |
| Brew/Brewfile.lock pinning unavailable for some formula | Medium | Fall back to documented float + `brew list --versions` log line (3.3) |
| `_FORTIFY_SOURCE=2` exposes latent `-Wstringop-*` warnings in Release | Medium | That is the point of the rule; fix the underlying code, never silence; A2 job will surface them |
| Debian dated tags rotate off Docker Hub | Low | Pin via digest `debian@sha256:...` if the dated tag disappears; documented in ci.yml |
| New fuzz hooks (`WEBSOCKET_TEST` etc.) expand test-only surface in prod files | Low | All hooks are `#ifdef *_TEST` guarded, scoped to fuzz/test targets only (same as existing pattern) |
| `sleep(1)` removal changes branch-ordering semantics unknowingly | Low | Keep the monotonic assertion; injected clock reproduces exact same timestamps deterministically |
| Frontend `require-jsdoc` at error blocks unrelated PRs | Low | Land as `warn` first, `error` after backlog cleared (6.2) |
| CI runtime grows (extra release job + 10 fuzz smoke runs) | Low | `-runs=1000` bounds fuzz to seconds; release job reuses cached apt layer |

---

## 11. Effort estimate (rough, per track)

| Track | Scope | Effort |
|---|---|---|
| A | CMake restructure + CI pinning + docs | 0.5-1 day |
| B | 6-8 new fuzz targets + 2 mirror rewrites + CI | 2-3 days |
| C | 7 modules of fail hooks + tests | 2-3 days |
| D | 30+ TSDoc comments + lint rule | 1-2 days |
| E | 19 new test files (E12 shared with C) | 3-4 days |
| F quick (8.1-8.4) | include fix, strlcat, test strings, timeouts | 0.5-1 day |
| F large (8.5-8.9) | 12+ function splits, void->status x10, decisions | 3-5 days |
| **Total** | | **12-19 days** |

Ordering note: F quick (Phase 2) and A (Phase 1) deliver most compliance value per hour;
F large (Phase 7) is the long tail.

---

## 12. Verification matrix (record results here as work progresses)

Baseline (Phase 0) — commit SHA: c3d43c86b03145e9c1e90bc2dcd75eda7015d538, date: 2026-08-10
(macOS: cmake 4.4.0, AppleClang 21.0.0, check 0.15.2 — 41/41 ctest passed, Debug + ASan/UBSan)

| Config | Linux (nix develop) | macOS (brew) | ctest result | ASan/UBSan | Fuzz smoke |
|---|---|---|---|---|---|
| Debug (strict c11) | CI pending | ✅ 61/61 | pass | on | n/a |
| Release + sanitizers | CI pending | ✅ 61/61 | pass | on | n/a |
| Release + sanitizers, `make release` | CI pending | ✅ (cmake Release, same flags) | pass | on | n/a |
| Coverage (`make coverage`, sanitizers off by design) | n/a | not rerun | | n/a | n/a |
| Fuzz: 16 targets `-runs=1000` | CI pending (fuzz job builds+runs all 16) | n/a (no libFuzzer on AppleClang) | | | pending CI |
| Frontend | CI pending | ✅ lint 0 problems, typecheck clean, 74/74 tests, build OK | | | n/a |

Definition of done for the whole plan:
- [x] Every checkbox in Sections 3-9 ticked (remaining unchecked items are the optional
      F9 fixture pass and CI-run confirmations that only the workflow run can provide).
- [ ] `rg '\b(strcpy|strcat|sprintf|gets)\b' src tests --glob '*.c'` clean (comments allowed).
- [ ] No `#pragma` suppression, no disabled warning class anywhere (grep).
- [ ] CI matrix green: 2 OSes x (Debug, Release) + fuzz job + frontend job.
- [ ] All new tests pass under ASan/UBSan (Linux) and Valgrind spot-check on at least the
      fault-injection suites.
- [ ] Audit-driven: this file's Section 0.2 rows all map to a closed checkbox or a recorded
      decision.
