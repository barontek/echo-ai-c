# Known pre-existing flake: test_web_fetch under parallel load

**Date:** 2026-08-12
**Reporter:** file-length compliance pass (S1-S6/T1-T2); flagged during full-suite verification
**Status:** pre-existing, NOT caused by the refactors; root cause unconfirmed, see below

## Symptom

`test_web_fetch` fails sporadically when the full suite runs in parallel (`ctest -j$(nproc)`):

```
tests/tools/test_web_fetch.c:178:F:challenge_fallback:test_retry_replaces_challenge_body:0:
Assertion 'replaced == 1' failed: replaced == 0, 1 == 1
```

Observed failure rate on the refactored tree: ~2/3 of full parallel runs, 0/N in isolation
(10/10 standalone), 0/3 at `-j2`, 0/2 and 2/3 at `-j20` across batches — load-dependent,
not deterministic.

## Evidence that it predates the refactors

- Failing code path is `web_fetch_test_retry_challenge` -> `retry_with_impersonator_binary`
  -> `fetch_via_impersonator` (src/tools/web_fetch.c:128-238, 240-269, 552+). It forks the
  fake `curl-impersonate-chrome` script and captures stdout; it does NOT touch html_extract
  or any file modified by the compliance splits.
- `git log -- src/tools/web_fetch.c` shows the file unmodified since 76f2fd5, the commit
  before this pass started.
- Baseline reproduction: a worktree at 76f2fd5 (pre-refactor) built with the same flags
  (`-DENABLE_SANITIZERS=ON`) failed the identical test under the full parallel suite
  (`59/61 Test #44: test_web_fetch ***Failed`, 98% passed). The baseline's flake rate was
  lower, consistent with its different suite composition/load profile.

## Failure modes consistent with the evidence

`replaced == 0` means one of (web_fetch.c:250-256): challenge detection failed
(deterministic, ruled out), `binary_on_path` failed (deterministic, ruled out), or
`fetch_via_impersonator` returned NULL: pipe()/fork() failure (fork EAGAIN under the
memory pressure of ~20 concurrent ASan test processes), the 10 s subprocess deadline
being exceeded under load, or a child-status check failure. Root cause was not pinned
down — the fork-pressure reproduction attempt was inconclusive and was stopped.

## Handling

- Not a regression from this pass; no code change was made here.
- Do NOT mark the split verification as red on this account; it is a tracked known issue
  rather than a pass failure.
- Suggested follow-up (not done): reproduce with strace/fork-counting on `fetch_via_impersonator`
  under parallel load, or rerun with `timeout_s` raised to distinguish deadline-exceeded
  from fork-failure.
