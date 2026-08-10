AGENTS.md — C Development Rules for Echo AI

This file governs how an AI coding agent writes, edits, and reviews C code in this repository. It's not a style preference doc — every rule exists to prevent a specific class of bug. Violations are build failures, not nitpicks.

Environment and toolchain

- Linux (NixOS): all builds and Check test runs happen inside `nix develop`. Never assume system-installed toolchain versions — if `nix develop` isn't active, the agent should not run `gcc`/`make`/`cmake` directly; it should either enter the shell first or flag that it's missing.
- macOS: use the project's Homebrew-based toolchain. The exact formula set is pinned in `Brewfile` at the repo root: `check`, `cjson`, `cmake`, `curl`, `libuv`, `openssl@3`, `pkgconf`, `sqlite` (install with `brew bundle --file=Brewfile`). Homebrew cannot version-pin unversioned formulae, so versions float; `brew list --versions` output is recorded in CI logs for traceability. Sanitizers are AppleClang's (bundled with Xcode), same ASan/UBSan flags as Linux; LeakSanitizer is unsupported on macOS, so CI sets `ASAN_OPTIONS=detect_leaks=0` — that is the one sanctioned deviation. Confirm clang version supports the same sanitizer flags as the Linux build before running anything.
- Other/CI: any environment outside the two above must have its toolchain versions pinned and documented before an agent runs a build in it — no ad hoc "whatever's installed" builds.
- An agent should never silently fall back to a system compiler when the expected dev shell isn't active — that's a "stop and ask" situation, not a "just try it" one.

macOS portability

Never rely on glibc's transitive includes. Every POSIX function used must have its declaring header included explicitly in that file.

- Known trap: `kill(2)`/`SIGKILL` are declared in `<signal.h>` — on Linux, `_GNU_SOURCE` + `<sys/wait.h>`/`<unistd.h>` pull them in transitively, on macOS they don't, and the result is `-Wimplicit-function-declaration` (an error under `-Werror`) only on the macOS CI runner. `bash.c`, `git.c`, `python_execute.c`, `server.c`, `web_fetch.c` all use `kill` — each must `#include <signal.h>`. If CI passes on Linux and fails with an undeclared function on macOS, grep the file for the missing include instead of patching flags.
- Same trap, `<unistd.h>`: `fork(2)`, `pipe(2)`, `close(2)`, `read(2)`, `write(2)`, `unlink(2)`, `access(2)`, `getcwd(3)`, `sleep(3)`/`usleep(3)` are declared there and glibc exposes them through other std headers, but macOS clang does not. Any file calling these must `#include <unistd.h>` itself. Historically the most-hit files were the tool subprocess helpers (`bash.c`, `python_execute.c`, `git.c`, `web_fetch.c`) — when adding a new subprocess- or fd-handling function, include `<unistd.h>` + `<signal.h>` + `<sys/wait.h>` up front rather than waiting for the macOS runner to complain.

Build flags

Build flags below assume the environment from the section above is active (`nix develop` on Linux, the pinned macOS toolchain otherwise). Every C target must compile clean under:

`-std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g`

- If a warning can't be fixed immediately, it gets a `// TODO(reason):` comment and a tracked issue — never silenced with a blanket pragma.
- Release builds add `-O2 -D_FORTIFY_SOURCE=2`; sanitizers stay on in CI even for "release" test runs.
- Never disable a warning class to "get it compiling." Fix the underlying issue or ask.

Memory ownership

- Every `malloc`/`calloc`/`realloc`/`strdup` call has a documented owner. If a function returns allocated memory, name it so (`_alloc`, `_dup`, `_new`) and state who frees it.
- No implicit ownership transfer through global state. If a struct owns a pointer, its `_destroy`/`_free` function frees it — no exceptions.
- Every allocation has a free on every exit path, including error paths. Prefer a single `goto cleanup;` block per function over duplicating frees across early returns.
- Double-free and use-after-free get the same audit rigor as any security bug.

Error handling

- Every function that can fail returns a status (error code or enum) — never a bare `void` with a "should work" assumption.
- Every return value from libc, syscalls, and third-party calls is checked.
- No silent failure paths — propagate the error or log-and-abort with context, never swallow it.
- Errors carry context (what operation, what input), not just an errno.

No undefined behavior

- No signed integer overflow relied on for wraparound.
- No strict-aliasing violations.
- No use of uninitialized memory — every local gets an explicit initializer.
- No out-of-bounds pointer arithmetic "because it'll probably still be valid" — bounds checked before dereference.
- No `gets`, `strcpy`, `strcat`, `sprintf` — use bounded equivalents (`strlcpy`, `snprintf`) and check truncation.

Structure and headers

- One header per module, matching `.c`. No circular includes. Documented exception: the tool modules under `src/tools/` share one subsystem contract via `tool.h` and `registry.h`; per-tool headers would only duplicate those factory declarations, so they are intentionally not generated. Everywhere else the one-header-per-module rule applies (`src/llm/factory.h` and `src/llm/ollama.h` were added in the 2026-08 compliance sweep).
- Include guards on every header, consistent style repo-wide.
- No function longer than ~60 lines without a strong reason.
- Public functions carry doc comments stating ownership and failure modes — the exact requirements are in the Documentation standards section below.

Code style

File size

- No hard line limit, but treat 300-800 lines as comfortable and 1000+ as a signal to split.
- Split along responsibility boundaries, not arbitrary line counts. If you can't describe what a file does in one sentence without "and," it's doing too much.
- Mirror module boundaries with compilation units (parser.c, lexer.c, hashtable.c) instead of letting a catch-all utils.c absorb everything.
- A file with 15+ functions is usually mixing concerns even if each function is short.
- A large pile of static helper functions backing one public function is a good candidate to split off with that function.

Functions

- Functions should be short and do one thing well.
- Minimize nesting depth. Deep nesting is usually a sign the function should be broken up or the logic simplified.
- One statement per line. One data declaration per line, no comma-chained declarations, so each item can carry its own comment if needed.

Naming

- Short, terse names are fine for local variables with small scope (loop counters, short-lived temporaries).
- Use descriptive names for anything with wider scope (globals, function names, public API).
- Avoid typedef'ing structs or pointers just to hide what they are. A reader should be able to tell a pointer is a pointer.

Comments

- Comment WHY the code does something, not WHAT it does. The code itself should make the "what" clear. Every non-obvious line gets a short comment explaining why, not what.
- Avoid comments inside a function body. If a function is complex enough to need inline explanation, that's usually a sign it should be split up.
- Put explanatory comments at the top of a function: what it does, and why, if that's not obvious from the name and signature.
- Don't write comments that just restate the function signature.

General philosophy

- Boring is good — the obvious, explicit version of a function beats a clever one-liner with pointer tricks.
- Consistency matters more than which specific convention you pick. Choose a style and apply it uniformly across the project.
- Prefer clarity over cleverness. Code that looks obvious after 20 straight hours of staring at a screen is doing its job.

Documentation standards

Two styles, one contract rule: the header or type signature carries the contract; the implementation carries the "why." C uses kernel-doc style, TypeScript/React uses TSDoc style.

C — kernel-doc style

Use kernel-doc style for all public functions (declared in `.h` files) — plain-English field names, matches the project's procedural/no-OOP conventions:

```
/
 * db_open - open a connection to the database
 * @path: null-terminated path to the SQLite file
 * @flags: DB_READONLY or DB_CREATE
 *
 * Return: pointer to an allocated db_conn_t, or NULL on failure with
 * errno set. Caller must release the connection with db_close().
 */
db_conn_t *db_open(const char *path, int flags);
```

Where docs live:

- Headers (.h) carry the contract. Every public function gets a doc comment above its declaration in the header — this is what someone integrates against without reading the .c file.
- Implementation (.c) carries the "why," not the "what." Inline comments explain non-obvious decisions, invariants, or workarounds — never restate what the code already says.
- File-level header at the top of every .c/.h: one or two lines on what the module is responsible for and what it depends on.

```
/*
 * db_crypto.c - Fernet-based field-level encryption for session records.
 * Depends on: sqlite3.
 */
```

Non-negotiable per function (C has no GC, no exceptions — the compiler won't catch you getting these wrong):

1. Ownership — who allocates, who frees. Any returned pointer: does the caller own it?
2. Lifetime — if a returned pointer becomes invalid after some other call (e.g. a free/realloc elsewhere), say so explicitly.
3. NULL behavior — does the function accept NULL args? Can it return NULL, and when?
4. Thread-safety — safe to call concurrently? Does it touch global/static state?
5. Error signaling — return code vs errno vs out-param. State which one, and use it consistently across the whole codebase.

TypeScript/React — TSDoc style

Use TSDoc-style comments for exported functions, types, and components — this is what VS Code/hover-tooltips and most TS tooling actually parse:

```
/
 * Fetches and decrypts a session record from the local API.
 *
 * @param sessionId - UUID of the session to load.
 * @param signal - AbortSignal to cancel the request on unmount.
 * @returns The decrypted session, or null if the session doesn't exist yet.
 * @throws {UnlockRequiredError} if the vault hasn't been unlocked this run.
 */
async function loadSession(sessionId: string, signal?: AbortSignal): Promise<Session | null>;
```

Where docs live:

- Types/interfaces carry the contract. In TS, the type signature already documents a lot (shape, optionality) — don't restate types in prose. Document what the type itself can't express: valid ranges, invariants between fields, when an optional field is actually required in practice.
- Component doc comment goes above the component, not scattered across individual props — describe what the component owns/renders and what triggers a re-render, not a line-by-line prop list (the props type already is that list).
- Hooks get a doc comment stating what they subscribe to and what they clean up — this is the TS equivalent of C's ownership/lifetime problem.

```
/
 * useSessionStream - subscribes to the SSE stream for a running session.
 * Cleans up the EventSource on unmount or when sessionId changes.
 */
function useSessionStream(sessionId: string): { messages: Message[]; isStreaming: boolean };
```

Non-negotiable per exported function/hook (the compiler catches shape, not intent):

1. Nullability beyond the type. `T | null` tells you it *can* be null — the comment says *when*/*why* (not found vs not loaded yet vs intentionally cleared).
2. Async failure mode. Does it throw, return a Result-like type, or swallow errors? State it — `Promise<T>` alone doesn't tell you.
3. Effect ownership. For hooks: what does it subscribe to, and what's torn down on cleanup? Uncleaned subscriptions are this codebase's version of a memory leak.
4. Mutation vs new reference. Does the function mutate its argument, or return a new object/array? Matters for React's referential-equality re-render checks.
5. Server vs client boundary. If a function only works in one context (browser API, Node-only, requires a specific provider context), say so — TS won't catch this.

General rules (both languages)

- Document the interface, not the implementation. If a comment needs to change every time the function body changes without its behavior changing, it's in the wrong place.
- Don't restate what the code already says (`// increment i` above `i++` is noise; `// id: the id` above `id: string` is noise) — comment intent, not mechanics. Don't document what the type system already says; explain constraints the type can't encode instead.
- Update the doc comment in the same commit as the code change. Stale docs are worse than no docs.
- One README per module/subsystem (C) or per feature/route directory (TS), not per component — that's where "why does this file exist" gets answered, not in scattered function comments.
- Consistency over cleverness: one comment format, enforced everywhere — TSDoc tags used the same way project-wide, enforced by a lint rule if the project grows past a handful of contributors.
- When C is touched by non-C tooling (bindings, agent code review), be extra explicit about ownership/lifetime in headers — that's the bug class invisible to a reviewer unless it's spelled out.
- For TS, be explicit about effect cleanup and error-throwing behavior in doc comments — those are the two things an agent doing code review is most likely to get wrong silently.

Testing

- Every new function with a non-trivial contract gets a Check unit test, run through the project's existing C test host.
- Every bug fix includes a regression test that would have caught it, added before the fix is marked resolved — the agent demonstrates it fails on the old code and passes on the new code, doesn't just assert it.
- Fuzz targets (libFuzzer or AFL++) required for any function parsing external input (files, network data, session blobs, tool output).
- Sanitizer-clean Check runs are a merge requirement, not optional CI noise.

Test file organization

- Mirror the source tree: one test file per source file being tested, named to match (`foo.c` -> `test_foo.c` or `foo.c` -> `foo_test.c`, pick one convention and stick to it).
- Don't let one test file grow to cover multiple unrelated source files just because it's convenient. Split it the same way you'd split a source file that's doing too much.
- Group related test cases together with a clear naming pattern, e.g. `test_<function>_<scenario>` (`test_parse_input_empty_string`, `test_parse_input_null_pointer`).

Size and scope

- Same rough guidance as source files: a test file that's grown past ~800-1000 lines is worth splitting, usually by feature or by which function/module is under test.
- One test function should test one behavior. If a single test function is asserting many unrelated things, split it.
- Prefer many small, focused tests over few large ones that try to cover everything at once. A failing test name should tell you what broke without reading the test body.

Independence and repeatability

- Tests should not depend on execution order or on state left behind by another test. Each test should set up what it needs and clean up after itself.
- Avoid shared mutable global state between tests unless it's reset in setup/teardown for every test.
- Tests should be deterministic. Avoid relying on timing, uninitialized memory, or system-specific behavior unless that's specifically what's being tested.

What to test

- Cover the normal case, boundary conditions, and error/failure paths (null pointers, zero-length input, allocation failure), not just the happy path.
- Test behavior and output, not implementation details. Tests that assert on internal state make refactoring painful without adding much safety.

Test comments

- Same rule as source: comment why a test exists or why an edge case matters, not what the assertion does line by line.
- If a test is a regression test for a specific bug, note that in a short comment (or the test name) so future readers know why it exists.

Check framework conventions

- One TCase per module or behavior area — never one flat suite for the whole codebase.
- Test names state the specific claim being falsified: test_unlock_rejects_wrong_password, not test_unlock.
- Use ck_assert_* typed macros, never bare assert() — they print expected vs. actual on failure instead of just "condition false."
- Fixtures via tcase_add_checked_fixture(tc, setup, teardown) for per-test state.
- tcase_set_timeout(tc, N) on any test touching I/O, locking, or anything that can hang — fork mode catches crashes and signals, not infinite loops.
- Debugging a failure: set CK_FORK=no before attaching gdb or valgrind, since fork mode hides both from the debugger. Use CK_RUN_SUITE / CK_RUN_CASE / CK_INCLUDE_TAGS to isolate one test instead of commenting code out.
- Build via CMake + pkg_check_modules(CHECK REQUIRED check), registered with ctest per test binary — Check has no gtest-style auto-discovery, so each suite binary needs its own add_test().

Verification discipline

- An agent's claim that something "works" or "is fixed" isn't sufficient — show the actual failing Check test before the fix and the passing one after, and for memory bugs, a clean ASan/UBSan/Valgrind run.
- `git stash` and rerun the previous behavior when a fix is claimed, to confirm the bug reproduces on old code and is gone on new code.
- Audit findings — from the agent itself or a second reviewing agent — aren't accepted at face value; verify via actual code path tracing with specific line numbers before marking fixed.
- No fix is "done" until it's run under sanitizers at least once.

Fault-injection testing (allocation-failure regression tests)

This project's most common bug shape has been: a function allocates via `str_dup`/`calloc`/`malloc`, doesn't check the result, and increments a count or commits a struct before confirming every allocation succeeded. Fixing this class of bug requires a test that can actually force an allocation to fail — a real OOM isn't reproducible on demand, so we fake it instead.

The pattern, in order of how much of the target function's surrounding code it needs:

Simple case — a single compilation unit, no external dependencies.
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

Harder case — the function under test pulls in real dependencies you don't want in a unit test (a DB, an LLM provider, 20+ transitive tool includes).
Two techniques, used together as needed:

- Forward-declare and `#define` around a specific call, so the test compilation unit substitutes a minimal mock (a stub struct with just the function pointers the code under test actually calls — see how `tool_delegate.c`'s test mocks `get_provider` with a stripped-down `LLMProvider` that only implements `chat`/`destroy`).
- A test-only guard around the heavy init path, when the dependency is baked into initialization rather than a single call (see `registry.c`'s `REGISTRY_TEST` guard, which skips wiring up the full tool registry so the test binary doesn't have to link everything registry.c would normally pull in).

The goal in both cases is the same: isolate the allocation logic from the I/O/provider logic so the fault-injection test only exercises the part that can actually have this bug — you don't need a live LLM or DB to prove a `str_dup` failure is handled correctly.

When to reach for this:
Any function that allocates more than one thing and then commits a count, index, or struct pointer based on those allocations succeeding. If you're writing or reviewing such a function and it doesn't have a fault-injection test, that's the same gap that caused the `metrics.c`, `semantic_search.c`, and `tool_delegate.c` bugs — write the test before considering the function done, not after something breaks.

Known gaps as of this sweep:
All allocation-safety paths in `metrics.c`, `config.c`, `memory.c`, `session_manager.c`, `change_tracker.c`, `semantic_search.c`, `tool_delegate.c`, and `registry.c` are covered. Any new module with multi-allocation commit logic should get this same treatment before merge, not discovered later via a production crash.

When to stop and ask

- If a change would require suppressing a sanitizer, disabling a warning, or skipping a test to land — stop and ask, don't route around it. C provides no safety net; the agent's job is to be that net.
- When in doubt about a memory-safety tradeoff, ask rather than guess.
