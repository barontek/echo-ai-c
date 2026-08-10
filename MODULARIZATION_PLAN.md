# Modularization Plan — Step 1: Extract branch subsystem from session_manager.c

Scope: split `src/session/session_manager.c` (2191 lines) by extracting the
branching subsystem into a new module `src/session/session_branch.c` +
`src/session/session_branch.h`. Pure code movement — zero behavior change.

## What moves (verbatim, zero behavior change)

16 functions from `src/session/session_manager.c`, lines 1292-1935, plus their
doc comments and the invariant comment block (lines 1270-1290):

| Functions | Lines |
|---|---|
| `branch_mint_id`, `branch_now_iso`, `branch_list_ensure`, `branch_active_created_at`, `branch_next_created_at`, `branch_set_active_created_at`, `branch_snapshot_messages`, `branch_record_create`, `branch_record_snapshot_live`, `branch_record_find`, `session_truncate_messages` | 1292-1496 |
| `session_manager_fork_branch` | 1498-1651 |
| `session_manager_switch_branch` | 1653-1743 |
| `session_manager_tag_message` | 1751-1792 |
| `branch_group_chain_count`, `session_manager_branch_info_alloc` | 1798-1935 |

After the move `session_manager.c` drops to ~1550 lines. The `SM_UPSERT` enum
(line 787) and statics `load_session_locked` / `save_session_core_locked` STAY
in session_manager.c — but the moved code's 7 direct call sites to those
statics are substituted with the behavior-identical public wrappers
`session_manager_load_session_nolock` / `session_manager_save_session_nolock`
(which forward to the same statics; `SM_UPSERT` is the only save mode branch
code uses, and the nolock wrappers' arg validation matches what the branch
entry points already check up front). Verified via diff of the moved block:
identical to the original modulo exactly these 7 substitutions.

## New files

### src/session/session_branch.h
- File-header comment: module responsibility + deps.
- Move from session_manager.h: `SessionManagerForkResult` typedef + doc
  (lines 239-248) and the 4 public functions with their kernel-doc comments
  (`session_manager_fork_branch`, `switch_branch`, `tag_message`,
  `branch_info_alloc`) — docs move verbatim.
- Includes session_manager.h (SessionManager type; also for the Session
  struct) and message.h (Message in SessionManagerForkResult).

### src/session/session_branch.c
- File header (two-line responsibility + deps), `#define _GNU_SOURCE`
  (asprintf).
- Includes: stdlib.h, string.h, time.h (clock_gettime, nanosleep,
  localtime_r, strftime), cjson/cJSON.h, session_branch.h, session_manager.h,
  session.h, message.h, utils/string_utils.h. All POSIX decls from explicitly
  included headers (macOS portability rule).
- Test hook block, AGENTS.md §11.2 pattern:

  ```c
  #ifdef SESSION_MANAGER_TEST
  char *sm_test_strdup(const char *s);
  #define str_dup sm_test_strdup
  #endif
  ```

  No realloc define — branch code never calls realloc (verified).
- The 16 functions moved verbatim.

## Modified files

1. **src/session/session_manager.c** — delete lines 1270-1935 (invariant
   comment + 16 functions); drop `static` from `sm_test_strdup` only; update
   file-header comment (drop "branch fork/switch"). All other test scaffolding
   stays put (bind/encrypt/oauth defines never fire in the branch TU).
2. **src/session/session_manager.h** — remove 4 branch declarations +
   `SessionManagerForkResult`; file header points to session_branch.h.
3. **Callers** (add `#include "session_branch.h"`):
   - src/server/routes/routes_session.c
   - src/server/routes/routes_ws.c
   - tests/routes/test_routes_session_handlers.c
   - tests/routes/test_routes_ws.c
   - tests/session/test_session_manager.c
4. **CMakeLists.txt** — add `src/session/session_branch.c` to
   `add_executable(echo-ai ...)`.
5. **tests/session/CMakeLists.txt** — add `../../src/session/session_branch.c`
   to the `test_session_manager` sources. `SESSION_MANAGER_TEST=1` is a
   per-target define, so it already applies to the new TU.

## Why fault-injection tests survive unchanged

`test_fork_branch_oom_leaves_session_unchanged` and
`test_tag_message_oom_leaves_session_unchanged` arm `set_alloc_fail(5)`. The
counter (`sm_alloc_counter`) stays file-local in session_manager.c; the shared
non-static `sm_test_strdup` wrapper reads it, so str_dup call ordering across
the fork/tag path is identical to today. No test edits needed.

## Verification protocol (AGENTS.md §8)

1. Baseline: `ctest --test-dir build --output-on-failure` green before
   touching anything (build: Apple clang, Debug, sanitizers ON).
2. Implement the split.
3. `cmake --build build` — clean under `-Wall -Wextra -Wpedantic -Werror
   -fsanitize=address,undefined`.
4. `ctest --test-dir build --output-on-failure` — full suite green; confirm
   test_session_manager (all tc_branches cases + both OOM tests) and the two
   routes test binaries.
5. `git stash` → rerun ctest → `git stash pop` → rerun: proves old and new
   code produce identical results.
6. No new alloc/free paths → no new fault-injection test needed; this step is
   reorganization only.

## Risks

- **Docs drift** — doc comments move verbatim; both file headers updated in
  the same commit.
- **Stale include** — any file calling branch API without session_branch.h
  fails at compile time (implicit declaration under -Werror); compiler catches
  every missed caller.
- **Test counter semantics** — the single `static` drop on `sm_test_strdup` is
  the only functional delta in test builds; covered by step 5.
