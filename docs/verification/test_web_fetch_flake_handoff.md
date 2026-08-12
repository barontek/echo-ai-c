# HANDOFF: test_web_fetch parallel-load flake — investigation status

**Date:** 2026-08-12
**Purpose:** everything known about the intermittent `test_web_fetch` failure, so a fresh
session can pick up the root-cause hunt without re-discovering any of it.
**Companion doc:** docs/verification/test_web_fetch_parallel_flake.md (record of the
evidence). This file adds the investigation log and concrete next steps.

## Symptom

Under the FULL parallel ctest suite only:

```
tests/tools/test_web_fetch.c:178:F:challenge_fallback:test_retry_replaces_challenge_body:0:
Assertion 'replaced == 1' failed: replaced == 0, 1 == 1
```

The failed test writes a fake `curl-impersonate-chrome` /bin/sh script, prepends its
mkdtemp'd dir to PATH, and calls `web_fetch_test_retry_challenge` with a challenge body
(`"Title: Just a moment...\n"`). `replaced == 0` means the retry did not replace the body.

## Reproduction recipe (all verified)

```bash
# NEVER fails (run alone, or -j anything with only this test):
nix develop -c bash -c 'cd build && ctest -R test_web_fetch --repeat until-fail:10'

# Fails ~2/3 of runs at full parallelism:
nix develop -c bash -c 'cd build && ctest -j$(nproc)'
```

Observed rates: `-j2` 0/3 failures; `-j20` 2/3 in one batch, 0/2 in another; full suite
repeatedly ~2/3. Timing per run ~1.2 s in isolation; the failing run takes ~1.4-2.1 s
(slightly longer than a passing run under load).

## Code path under test

- Test: tests/tools/test_web_fetch.c:160-190 (`test_retry_replaces_challenge_body`);
  fake binary writer at :63-70; PATH setup at :72-82; per-test mkdtemp fixture at :32-57.
- Unit under test (unchanged since 76f2fd5 — see Evidence):
  - src/tools/web_fetch.c:552  `web_fetch_test_retry_challenge`
  - src/tools/web_fetch.c:244-269 `retry_with_impersonator_binary`
  - src/tools/web_fetch.c:128-238 `fetch_via_impersonator` — pipe(2) + fork(2) +
    poll(2) loop with 100 ms slices + waitpid(WNOHANG); deadline = timeout_s*1000 =
    10 000 ms (timeout_s=10 from the test); drains the pipe after child exit; fails on
    `!WIFEXITED(status) || WEXITSTATUS(status) != 0 || buf.len == 0`.

`replaced == 0` requires exactly one of (deterministic causes ruled out):

1. `looks_like_challenge` returned 0 — input is fixed, ruled out.
2. `binary_on_path` returned 0 — PATH is test_dir + saved_path, ruled out.
3. `fetch_via_impersonator` returned NULL:
   a. `pipe(2)` failure
   b. `fork(2)` failure (EAGAIN) — prime suspect, see Hypotheses
   c. 10 s deadline exceeded under load (loop only advances `elapsed_ms` on poll
      timeout or after a read; EINTR paths skip the increment — review carefully)
   d. `http_buffer_append` failure
   e. child exited non-zero or empty stdout (fake script is trivial; unlikely but
      exec of `/bin/sh` under load could fail)

## Evidence it is pre-existing (NOT caused by the refactors)

- `git log -- src/tools/web_fetch.c` → last touched at 76f2fd5, the commit before the
  file-length compliance work started.
- Baseline reproduction: worktree at 76f2fd5 built with `-DENABLE_SANITIZERS=ON` failed
  the identical test under the full parallel suite: `59/61 Test #44: test_web_fetch
  ***Failed` (98% passed). Baseline flake rate looked lower (1/4 then 1 in 10 in later
  batches) — likely because the baseline suite has a different load profile, not because
  of any behavior difference.
- The failing path never touches html_extract or any file the refactors split.
- Consequence: do NOT treat this as a regression. It is a tracked known issue.

## What has been tried (and results)

| Attempt | Result |
|---|---|
| 10x isolated runs | all pass |
| ctest -R test_web_fetch -j$(nproc) (alone) | all pass |
| -j2 full suite ×3 | 0 failures |
| -j20 full suite (two batches) | 2/3, then 0/2 |
| Full suite repeatedly | ~2/3 failure rate |
| Fork-hammer during isolated runs (300× `sleep 0.3 &` loops) | inconclusive; the experiment was aborted (too long-running), no failure observed |
| gdb in the nix shell | not available (`gdb: command not found`) — use CK_FORK=no + `CK_RUN_CASE` instead of gdb, or add temporary instrumentation |

Environment facts: NixOS dev shell, gcc 15.2.0, ASan+UBSan enabled, 20 cores
(`-j$(nproc)` = 20), `ulimit -u` 62701, `/proc/sys/vm/overcommit_memory` = 0
(heuristic overcommit).

## Hypotheses, ranked

1. **fork(2) EAGAIN under memory pressure.** ~20 concurrent ASan test binaries each
   reserve large address spaces; overcommit_memory=0 + fork can fail with EAGAIN. The
   stub returns NULL silently (`fetch_via_impersonator` :136-141) → replaced == 0.
   Consistent with: never fails in isolation, rate rises with parallelism, no
   determinism.
2. **10 s subprocess deadline exceeded under CPU contention.** The loop's `elapsed_ms`
   accounting is suspect on the EINTR path (`if (pr < 0 && errno != EINTR)` falls
   through to the read without advancing the clock) and when `poll` returns early
   repeatedly with data. Less likely (a /bin/sh printf should complete in ms) but not
   eliminated.
3. Something in the waitpid/poll/read loop that only manifests under scheduler
   latency (e.g., child exits between waitpid and read with the pipe drained wrongly).
   Reading the loop once more with the EINTR/elapsed accounting in mind is cheap.

## Suggested next steps (in order of cost)

1. **Instrument the failure path temporarily** (remember to revert): add
   `fprintf(stderr, "wf retry fail: %s\n", strerror(errno))` at the pipe()/fork()/
   deadline returns in `fetch_via_impersonator`, rebuild, loop the full suite until it
   fails, read stderr from build/Testing/Temporary/LastTest.log. This alone should
   settle hypothesis 1 vs 2.
2. **Deadline test without code changes:** the test passes `timeout_s=10`; a quick
   hack — copy the failing test locally with `timeout_s=60` — if failures vanish, it
   was the deadline; if not, it is fork/exec.
3. **Watch the system during a failing run:** `dmesg | tail` for OOM-kills, and
   `cat /proc/sys/vm/max_map_count` + `ps -eLf | wc -l` at failure time.
4. If fork-EAGAIN is confirmed: the honest fix is to retry fork() on EAGAIN with a
   short sleep in `fetch_via_impersonator`, or to serialize the retry fetch (mutex)
   — plus a regression test that fakes EAGAIN via an LD_PRELOAD or a test-only hook
   that forces the retry path.
5. If the deadline is confirmed: fix the `elapsed_ms` accounting (advance on every
   iteration, not only on timeout/read) and/or raise the test's timeout.

## Files that will matter

- tests/tools/test_web_fetch.c (test + fake binary + fixture)
- src/tools/web_fetch.c:128-269 (the retry/impersonator path)
- docs/verification/test_web_fetch_parallel_flake.md (evidence record)
- .github/workflows/ci.yml (CI runs ctest -V, which will show this flake)
