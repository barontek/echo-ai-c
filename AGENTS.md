AGENTS.md — C Development Rules for Echo AI

This file governs how an AI coding agent writes, edits, and reviews C code in this repository. It's not a style preference doc — every rule exists to prevent a specific class of bug. Violations are build failures, not nitpicks.

0. Environment setup

- Linux (NixOS): all builds and Check test runs happen inside `nix develop`. Never assume system-installed toolchain versions — if `nix develop` isn't active, the agent should not run `gcc`/`make`/`cmake` directly; it should either enter the shell first or flag that it's missing.
- macOS: use the project's Homebrew-based toolchain (document the exact formula list here once locked in — e.g. `llvm`, `check`, sanitizer runtimes bundled with Xcode's clang). Confirm clang version supports the same sanitizer flags as the Linux build before running anything.
- Other/CI: any environment outside the two above must have its toolchain versions pinned and documented before an agent runs a build in it — no ad hoc "whatever's installed" builds.
- An agent should never silently fall back to a system compiler when the expected dev shell isn't active — that's a "stop and ask" situation, not a "just try it" one.

1. Build flags are non-negotiable

Build flags below assume the environment from Section 0 is active (`nix develop` on Linux, the pinned macOS toolchain otherwise).
Every C target must compile clean under:

`-std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g`

- If a warning can't be fixed immediately, it gets a `// TODO(reason):` comment and a tracked issue — never silenced with a blanket pragma.
- Release builds add `-O2 -D_FORTIFY_SOURCE=2`; sanitizers stay on in CI even for "release" test runs.
- Never disable a warning class to "get it compiling." Fix the underlying issue or ask.

2. Memory ownership must be explicit

- Every `malloc`/`calloc`/`realloc`/`strdup` call has a documented owner. If a function returns allocated memory, name it so (`_alloc`, `_dup`, `_new`) and state who frees it.
- No implicit ownership transfer through global state. If a struct owns a pointer, its `_destroy`/`_free` function frees it — no exceptions.
- Every allocation has a free on every exit path, including error paths. Prefer a single `goto cleanup;` block per function over duplicating frees across early returns.
- Double-free and use-after-free get the same audit rigor as any security bug.

3. Error handling is mandatory and uniform

- Every function that can fail returns a status (error code or enum) — never a bare `void` with a "should work" assumption.
- Every return value from libc, syscalls, and third-party calls is checked.
- No silent failure paths — propagate the error or log-and-abort with context, never swallow it.
- Errors carry context (what operation, what input), not just an errno.

4. No undefined behavior, ever

- No signed integer overflow relied on for wraparound.
- No strict-aliasing violations.
- No use of uninitialized memory — every local gets an explicit initializer.
- No out-of-bounds pointer arithmetic "because it'll probably still be valid" — bounds checked before dereference.
- No `gets`, `strcpy`, `strcat`, `sprintf` — use bounded equivalents (`strlcpy`, `snprintf`) and check truncation.

5. Structure and headers

- One header per module, matching `.c`. No circular includes.
- Include guards on every header, consistent style repo-wide.
- Public functions get a one-line doc comment on ownership of pointer args/returns and failure modes — not "what it does," but "who owns what, and how it fails."
- No function longer than ~60 lines without a strong reason.

6. Testing is part of the change, not a follow-up

- Every new function with a non-trivial contract gets a Check unit test, run through your existing C test host.
- Every bug fix includes a regression test that fails on the old code and passes on the new code — the agent demonstrates this, doesn't just assert it.
- Fuzz targets (libFuzzer or AFL++) required for any function parsing external input (files, network data, session blobs, tool output).
- Sanitizer-clean Check runs are a merge requirement, not optional CI noise.

7. Verification discipline

- An agent's claim that something "works" or "is fixed" isn't sufficient — show the actual failing Check test before the fix and the passing one after, and for memory bugs, a clean ASan/UBSan/Valgrind run.
- `git stash` and rerun the previous behavior when a fix is claimed, to confirm the bug reproduces on old code and is gone on new code.
- Audit findings — from the agent itself or a second reviewing agent — aren't accepted at face value; verify via actual code path tracing with specific line numbers before marking fixed.
- No fix is "done" until it's run under sanitizers at least once.

8. What "good C" means here, concretely

- Boring is good — the obvious, explicit version of a function beats a clever one-liner with pointer tricks.
- Every non-obvious line gets a short comment explaining *why*, not *what*.
- When in doubt about a memory-safety tradeoff, ask rather than guess.

9. What to do when unsure

If a change would require suppressing a sanitizer, disabling a warning, or skipping a test to land — stop and ask, don't route around it. C provides no safety net; the agent's job is to be that net.

10. Fault-injection testing pattern (allocation-failure regression tests)

This project's most common bug shape has been: a function allocates via `str_dup`/`calloc`/`malloc`, doesn't check the result, and increments a count or commits a struct before confirming every allocation succeeded. Fixing this class of bug requires a test that can actually force an allocation to fail — a real OOM isn't reproducible on demand, so we fake it instead.

The pattern, in order of how much of the target function's surrounding code it needs:

1. **Simple case — a single compilation unit, no external dependencies.**
   Redefine the allocator for test builds only, via a compile-time guard:

   ```
   #ifdef METRICS_TEST
   static int strdup_fail_at = -1;
   static int strdup_call_count = 0;
   static char *test_strdup(const char *s) {
       strdup_call_count++;
       if (strdup_call_count == strdup_fail_at) return NULL;
       return str_dup(s);
   }
   #define str_dup test_strdup
   #endif
   ```

   The test sets `strdup_fail_at` to the Nth call it wants to fail, calls the real function, and asserts: the function returns an error/NULL, nothing partial got committed (count unchanged, no dangling struct), and everything allocated before the failure point was freed. Then it resets and confirms normal operation still works.

   The test target opts into this by adding the compile definition only for its own build (e.g. in `tests/CMakeLists.txt`, not the main target), so production builds never see the fake allocator.

2. **Harder case — the function under test pulls in real dependencies you don't want in a unit test (a DB, an LLM provider, 20+ transitive tool includes).**
   Two techniques, used together as needed:
   - **Forward-declare and `#define` around a specific call**, so the test compilation unit substitutes a minimal mock (a stub struct with just the function pointers the code under test actually calls — see how `tool_delegate.c`'s test mocks `get_provider` with a stripped-down `LLMProvider` that only implements `chat`/`destroy`).
   - **A test-only guard around the heavy init path**, when the dependency is baked into initialization rather than a single call (see `registry.c`'s `REGISTRY_TEST` guard, which skips wiring up the full tool registry so the test binary doesn't have to link everything registry.c would normally pull in).

   The goal in both cases is the same: isolate the allocation logic from the I/O/provider logic so the fault-injection test only exercises the part that can actually have this bug — you don't need a live LLM or DB to prove a `str_dup` failure is handled correctly.

**When to reach for this:**
Any function that allocates more than one thing and then commits a count, index, or struct pointer based on those allocations succeeding. If you're writing or reviewing such a function and it doesn't have a fault-injection test, that's the same gap that caused the `metrics.c`, `semantic_search.c`, and `tool_delegate.c` bugs — write the test before considering the function done, not after something breaks.

**Known gaps as of this sweep:**
All allocation-safety paths in `metrics.c`, `config.c`, `memory.c`, `session_manager.c`, `change_tracker.c`, `semantic_search.c`, `tool_delegate.c`, and `registry.c` are covered. Any new module with multi-allocation commit logic should get this same treatment before merge, not discovered later via a production crash.
