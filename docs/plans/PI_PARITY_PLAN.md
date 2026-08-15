# Pi Parity Plan — bringing the four core tools to pi's contract

**Date:** 2026-08-15
**Source:** comparison of `earendil-works/pi` (HEAD, `packages/coding-agent/src/core/tools/{read,write,edit,bash}.ts` + `edit-diff.ts`) vs `src/tools/{read_file,write_file,replace_in_file,bash}.c`
**Repo:** /home/barontek/echo-ai-c
**Scope:** align read/write/edit/bash with pi's agent-facing contracts where they are strictly better, while keeping our safety layer, change-tracker undo, and atomic-write guarantees (pi has none of those; we do not regress them).

## Principles

1. **Adopt pi's contract, keep our safety net.** Schema and error semantics come from pi; `safety_check_path`, `safety_resolve_path_alloc`, `max_file_size`, change-tracker snapshots, and temp+rename atomic writes stay.
2. **Every behavior change ships with a regression test that fails on the old code** (per AGENTS.md verification discipline) and is green under ASan+UBSan.
3. **Multi-allocation commit sites get fault-injection tests** (per AGENTS.md fault-injection section): the multi-edit path in replace_in_file is exactly the bug shape that bit metrics.c/semantic_search.c/tool_delegate.c.
4. **LLM-facing contract changes go through the registry + `config.conf.example` + frontend renderers together** — a schema the TUI can't render is a half-shipped change.
5. Items marked `[defer]` are explicitly scoped out with a reason — never silently skipped.

---

## Phase 0 — rename `replace_in_file` → `edit` (pure refactor, no behavior change)

Pi calls this tool `edit`; the LLM-facing contract should match. This is a mechanical rename done first, so every later phase in this plan works on the final file names. No behavior, schema, or test-assertion changes here — only names and build wiring.

### T0 [rename] file, symbol, and registry rename
- **Files:**
  - `git mv src/tools/replace_in_file.c src/tools/edit.c`
  - `git mv tests/tools/test_replace_in_file.c tests/tools/test_edit.c`
- **Source (`src/tools/edit.c`):** file header comment; `replace_in_file_execute` → `edit_execute`; `replace_in_file_destroy` → `edit_destroy`; `tool_replace_in_file_create` → `tool_edit_create` (and its doc comment); `replace_in_file_test_set_fwrite_fail` → `edit_test_set_fwrite_fail`; `REPLACE_IN_FILE_TEST` guard → `EDIT_TEST`; log line `"replace_in_file: rename failed"` → `"edit: rename failed"`; and the contract name itself: `t->name = str_dup("edit")`.
- **Build wiring:** `CMakeLists.txt:74`; `tests/tools/CMakeLists.txt:100-112` (`test_edit` binary, source, `EDIT_TEST=1` define, ctest registration).
- **Registry (`src/tools/registry.c:100/:135`):** forward decl + registration call rename to `tool_edit_create`.
- **Safety contract (`src/safety/safety.c:221`, `safety.h:101/:151`):** the approval-list string `"replace_in_file"` → `"edit"` — this is the machine-readable tool key, must match the registry name exactly or the approval gate silently never applies to the tool.
- **Config + frontend:** `config.conf.example:165` list; `frontend/src/components/MessageList.tsx:66/:114/:139` — `case 'replace_in_file'` → `case 'edit'`.
- **Tests (`tests/tools/test_edit.c`):** test function names `test_replace_in_file_*` → `test_edit_*`; factory call sites; `tests/agent/test_safety.c:461` name array entry.
- **Not touched (historical record):** `docs/plans/AGENTS_COMPLIANCE_FIX_PLAN.md`, `docs/reviews/AGENTS_COMPLIANCE_REVIEW.md`, `docs/verification/2026-08-11_fix_evidence.md`, `docs/no_longer_in_use/` — these document findings against the old name at that point in time; rewriting them would falsify the record. New evidence and this plan use the new name.
- **Tests:** full `test_edit` + `test_registry` + `test_safety` green under ASan+UBSan; assert `tool->name == "edit"` and that the safety approval list lookup still gates the tool by its new key (test_safety covers the list; add one assertion in test_registry that the registered tool's name is `edit`).
- **Verify:** `make check` clean; evidence line in docs/verification/.

Note: `read_file` and `write_file` keep their names — pi's `read`/`write` are cosmetic equivalents and the user-facing rename request covers the edit tool only. Renaming those is a follow-up decision, not part of this plan.

---

## Phase 1 — read: offset/limit pagination with continuation hints

### T1 [read] offset/limit args + truncation notice
- **pi behavior:** `{path, offset?, limit?}`; text reads truncated to 8 KB / 4000 lines; on truncation the result appends `[Showing lines X-Y of N. Use offset=Y+1 to continue.]`; offset past EOF is an explicit error. Image support is out of scope (no vision path in this codebase yet — `[defer]`, see T1d).
- **Fix** (`src/tools/read_file.c`):
  1. Extend `parameters_schema`: `offset` and `limit` optional numbers (offset 1-indexed, mirroring pi).
  2. Keep the `max_file_size` hard cap (pi has none — ours is stricter by design; a policy-denied over-cap file stays denied rather than silently truncated).
  3. Apply offset/limit as line ranges after reading: `start = offset ? offset - 1 : 0`; error `"offset beyond end of file (N lines)"` (category `validation_error`) if start >= line count.
  4. Truncation: if no `limit` given, cap returned lines at a `READ_MAX_LINES` define (start at 4000 to match pi) and append the continuation hint with exact next offset. If `limit` given, honor it and append a `[N more lines in file...]` hint when the file continues.
  5. Fix the known LOW while here: compare the `fread` result against the expected size (C5 pattern from replace_in_file.c:113) — a file changed under us must not be silently half-returned. **This is a bug fix on its own and gets its own test (T1e).**
- **Tests** (`tests/tools/test_read_file.c`, new cases):
  - `test_read_file_offset_limit_pagination`: read a 20-line file with offset=6, limit=5; assert exact lines + hint `[9 more lines in file...]`.
  - `test_read_file_truncation_notice`: no limit; assert truncated output ends with the continuation hint and `nextOffset` math is right.
  - `test_read_file_offset_past_eof`: validation_error, message contains line count.
  - `test_read_file_offset_zero_defaults`: offset=0 behaves as no offset.
  - `test_read_file_short_read_reports_error` [T1e]: fault seam on fread (pattern of `replace_in_file_test_set_fwrite_fail`) — old code returns partial content as success (or NULL), new code errors.
- **Verify:** `test_read_file` green under ASan+UBSan; evidence in docs/verification/.

### T1d [defer] image reads (pi sends images as vision attachments)
- **Reason:** this codebase has no vision-capable LLM integration; MIME sniffing + image resize/attachment is a feature of its own, gated on a model that accepts images. Revisit when a vision provider is wired.

---

## Phase 2 — write: auto-create parent directories

### T2 [write] mkdir -p on parent before write
- **pi behavior:** write creates parent directories recursively; write serializes per-path via a mutation queue (see T5 — C-side decision there).
- **Fix** (`src/tools/write_file.c`):
  1. After `safety_resolve_path_alloc`, walk `dirname(resolved)` and `mkdir(2)` each missing component (`mode 0755`) up to the resolution root. Do **not** touch anything above the safety-resolved root — path-allowlist semantics must hold (a denied directory must not be created by a side effect).
  2. Keep everything we already do that pi lacks: change-tracker snapshot, `fwrite`/`fclose` result checks, `max_file_size` content cap.
- **Tests** (`tests/tools/test_write_file.c`):
  - `test_write_file_creates_nested_parents`: write to `tmp/sub/a/b/c.txt`; assert dirs exist and content correct.
  - `test_write_file_existing_dir_is_noop`: existing parents still write fine.
  - `test_write_file_mkdir_failure_reports_error`: seam on mkdir (pattern of the fwrite seam) — old code fails at fopen with `permission_denied`; new code must fail with the same category and no partial dirs left behind on the failing component.
  - `test_write_file_snapshot_still_taken`: ct_snapshot still invoked before write (regression guard for the undo path).
- **Verify:** `test_write_file` green under ASan+UBSan.

---

## Phase 3 — edit (renamed from replace_in_file): pi's edit contract (biggest item)

### T3 [edit] multi-edit batching, uniqueness enforcement, overlap detection
- **pi behavior:** `{path, edits: [{oldText, newText}, ...]}` — all edits matched against the **original** file, applied in reverse for stable offsets. Each oldText must be unique (error reports occurrence count); overlapping/nested edits error; empty oldText errors; a no-op result (replacement produced identical content) errors. Legacy single `{oldText,newText}` args are accepted and folded into `edits`.
- **Fix** (`src/tools/edit.c`, renamed in T0):
  1. Schema: `edits` array of `{old_string, new_string}`; accept legacy top-level `old_string`/`new_string` and fold into a one-element array (pi's `prepareEditArguments` does exactly this).
  2. **Uniqueness:** count occurrences of each old_string in the original content; >1 → `validation_error` with the count and "provide more context to make it unique" — replaces today's silent first-occurrence pick. This is the single highest-value behavioral change (agent determinism).
  3. Match all edits against the original buffer, detect pairwise overlap, apply in reverse order. Empty old_string and identical-result (no-op) errors, pi's exact messages.
  4. Error messages name the failing index: `edits[2]` vs the single-edit wording — models use this to self-correct.
  5. Keep atomic temp+rename (we beat pi here), and re-verify all allocations: the edit array copy + per-edit dup + result buffer is a new multi-alloc commit site → fault-injection tests required (T3d).
- **Tests** (`tests/tools/test_edit.c`, renamed in T0):
  - `test_edit_multiple_disjoint_edits`: 3 disjoint edits in one call, assert all applied.
  - `test_edit_duplicate_old_string_reports_count`: same text twice → error mentions "2 occurrences"; old code silently replaced the first — **regression test fails on old code**.
  - `test_edit_overlapping_edits_error`.
  - `test_edit_empty_old_string_error`.
  - `test_edit_noop_reports_error`: replace text with identical text → error.
  - `test_edit_legacy_single_args_still_work`: old `{path,old_string,new_string}` shape unchanged behavior.
  - `test_edit_edit_index_in_error_message`: 2-edit call with the 2nd unmatched; message contains `edits[1]`.
  - T3d `test_edit_oom_rollback`: fault seam on str_dup (compile guard already exists: `EDIT_TEST`) — force Nth allocation to fail mid-edit-array; assert error result, original file untouched, no partial commit.
  - Existing tests keep passing (first-occurrence semantics tests change to uniqueness semantics; that's an intended contract change, update them in the same commit).
- **Verify:** `test_edit` green under ASan+UBSan; Valgrind spot-check on the multi-edit path.

### T3a [edit] BOM + line-ending preservation
- **pi behavior:** strips a UTF-8 BOM before matching, restores it after; detects CRLF vs LF and restores the original ending in the written file.
- **Fix:** mirror `stripBom`/`detectLineEnding`/`normalizeToLF`/`restoreLineEndings` (edit-diff.ts:10-24, 247-249) — ~40 lines of pure functions in C with no deps, safe to add.
- **Tests:** `test_edit_crlf_preserved` (edit a CRLF file, written file still CRLF), `test_edit_bom_preserved`, `test_edit_crlf_old_string_match` (old_string with `\n` matches `\r\n` file — matching happens in normalized space).

### T3b [defer] fuzzy matching (pi's `normalizeForFuzzyMatch`)
- **Reason:** NFKC normalize + smart-quote/dash/space folding + line-granular replacement-preserving-unchanged-lines is the riskiest port (Unicode tables in C, correctness under ASan). Exact-match + uniqueness + actionable errors (T3) already fixes the dominant failure mode (models mis-copy text → duplicate/no-match errors tell them precisely what to fix). Schedule as a follow-up phase with its own plan if model edit-miss rates warrant it.

### T3c [defer] diff/patch in the result payload
- **Reason:** pi's `generateDiffString`/`generateUnifiedPatch` feed a TUI diff renderer we don't have. When the TUI migration (TUI_MIGRATION_PLAN.md) lands a diff view, add unified-patch output to the ToolResult then.

---

## Phase 4 — bash: per-call timeout + output spill

### T4 [bash] per-call timeout override
- **pi behavior:** `timeout` optional seconds, no default; validated (finite, >0, <= ~24.8 days).
- **Fix** (`src/tools/bash.c`):
  1. Add optional `timeout` to the schema. Semantics: absent → policy `max_execution_time` (today's behavior); present → must be >0 and capped at policy `max_execution_time` (hard cap preserves the safety layer — the LLM cannot escalate beyond policy, unlike pi which has no cap).
  2. Validate in `bash_execute`; invalid values → `validation_error` with the allowed range in the message.
- **Tests** (`tests/tools/test_bash.c`):
  - `test_bash_timeout_override_applied`: timeout=1 on a `sleep 5` command → timeout error, faster than policy default.
  - `test_bash_timeout_zero_rejected`: `validation_error`.
  - `test_bash_timeout_above_policy_capped`: request 9999s, assert command still killed at policy max (sleep-beyond-policy command; assert kill time ≈ policy max, not 9999s).
  - `test_bash_no_timeout_uses_policy_default` (existing behavior regression).

### T4a [bash] output spill to temp file instead of silent 64 KB truncation
- **pi behavior:** output truncated to last 8 KB / 4000 lines; full output saved to a temp file and the path included in the result; nonzero exit still returns partial output with exit code.
- **Fix** (`src/tools/bash.c`):
  1. Replace the fixed `result_buf[65536]` with a growable buffer; once it exceeds a `BASH_TAIL_BYTES` budget (8 KB start), keep only the **tail** (line-aware) and start spilling full output to a `mkstemp` temp file (`/tmp/echo-bash-XXXXXX` or `TMPDIR`-honoring).
  2. Result text appends `[Full output: <path>]` when spilled. Temp file is unlinked on... **decision:** pi keeps the file (model can be pointed at it); we keep it too but document that server exit cleans /tmp. Unlink-on-session-end is a follow-up if a temp-file lifecycle API exists — note in plan.
  3. Keep the exit-code mapping and process-group kill logic unchanged.
- **Tests** (`tests/tools/test_bash.c`):
  - `test_bash_large_output_spills_to_temp`: emit >8 KB; assert result ends with `[Full output: ...]`, the tail is the **last** lines, and the temp file contains the full output.
  - `test_bash_tail_is_line_complete`: tail starts at a line boundary.
  - `test_bash_spill_nonzero_exit_still_returns_output`: big output + exit 1 → both partial output and exit code present.
  - `test_bash_small_output_no_spill` (regression).
  - Temp-file leak check under the test's teardown (file exists → removed or explicitly left with documented lifecycle).

---

## Phase 5 — cross-cutting: contract plumbing

### T5 [defer] per-path write serialization (pi's `withFileMutationQueue`)
- **Reason:** pi serializes mutations because its agent loop is concurrent (multiple streams/TUI renders). This server's tool execution is already single-threaded per session (tool.h: execute is not thread-safe by contract, callers serialize). Revisit only if concurrent tool execution is added.

### T5a [plumbing] LLM-facing contract updates ship together
- `src/tools/registry.c`: no changes needed (same tool names); only schemas change via the tool `_create` functions.
- `config.conf.example:165` tool list: unchanged names.
- `frontend/src/components/MessageList.tsx` (lines 66/114/139): handle the new `edits` array shape in the `edit` tool renderers (fall back to legacy path display); show read's `offset`/`limit` in the call line; bash timeout suffix (pi renders `(timeout Ns)`).
- Tool descriptions in each `_create`: update to pi's phrasing (e.g. edit: "one or more targeted replacements, each old_string must be unique...").
- `tests/agent/test_safety.c:461` and `tests/tools/test_registry.c`: names unchanged, only re-run.

### T5b [docs] update after each phase
- Header doc comments per AGENTS.md kernel-doc rules (ownership/lifetime/error signaling already documented; add new failure categories).
- `docs/reviews/` gets a PI_PARITY_REVIEW.md once all phases land, mapping each pi behavior to ours with file:line.
- Update the known-gaps list in AGENTS.md (read_file fread gap closed by T1e; replace_in_file short-write already covered).

---

## Sequencing and effort

| Phase | Item | Effort | Depends on | Status |
|---|---|---|---|---|
| 0 | T0 rename replace_in_file → edit | S | — | [x] |
| 1 | T1 read offset/limit + T1e short-read fix | M | T0 (build wiring) | [x] |
| 2 | T2 write mkdir | S | — | [x] |
| 3 | T3 multi-edit/uniqueness + T3d fault tests | L | T0 | [x] |
| 3 | T3a BOM/CRLF | S | T3 (same file) | [x] |
| 4 | T4 bash timeout override | S | — | [x] |
| 4 | T4a output spill | M | — | [x] |
| 5 | T5a frontend/plumbing | S | all of the above | [x] |

Order: T0 (rename, everything else builds on the new names) → T2 (smallest, isolates mkdir semantics) → T4 (bash timeout, self-contained) → T1 (read) → T3 (edit, largest, last so its test churn is final) → T4a (spill) → T5a plumbing sweep.

**Definition of done (per phase):** tests green under ASan+UBSan, regression tests demonstrated failing on pre-fix tree where required (`git stash` per AGENTS.md), fault-injection tests for every new multi-alloc commit site, evidence archived in `docs/verification/`, boxes ticked in this plan.

---

## Status — 2026-08-15 execution

All non-deferred phases shipped in one pass. Evidence:

- **Build:** full tree builds clean under `-std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined` (ASan+UBSan confirmed linked in the test binaries).
- **T0 rename:** `src/tools/edit.c` + `tests/tools/test_edit.c` via `git mv`; registry, safety approval list, config.conf.example, MessageList.tsx, CMake wiring updated; `test_edit`/`test_registry`/`test_safety`/`test_safety_conf` green. `tool->name == "edit"` asserted in test_edit.
- **T2 write mkdir:** `write_file_ensure_parents` walks from the realpath'd workspace root, realpath-verifying every prefix; unrestricted mode gets plain mkdir -p. Tests: nested-parents, existing-parent-perms-untouched, mkdir-fault seam (`WRITE_FILE_TEST`), symlink-escape-refused (writes to `/etc/c.txt` refused).
- **T4 bash timeout:** optional `timeout` arg, whole-seconds validated, capped at policy max (no escalation). Tests: override applied (fails on pre-T4 code — policy 30s would swallow it), zero/fractional rejected, above-policy capped (9999s request dies at the 2s policy limit).
- **T1 read:** `offset`/`limit` windowing with pi-style continuation hints; `fread` short-read now an error (seam `READ_FILE_TEST`). Tests: pagination, truncation notice, offset past EOF, invalid windows, short-read regression.
- **T3 edit:** `edits[]` array contract + legacy folding; uniqueness enforcement with occurrence counts; overlap/no-match/no-op/empty-old_string errors naming `edits[N]`; reverse-order application; atomic write kept. T3d str_dup fault seam (`EDIT_TEST`): OOM mid-edit-array rolls back, file untouched, normal operation restored after. T3a BOM strip/restore + CRLF detect/preserve/normalized matching.
- **T4a bash spill:** output beyond 8 KB tail budget spills full output to a mkstemp temp file, result ends `[Full output: <path>]`, tail is line-boundary-aligned; spill failure is an explicit error (-4), never silent truncation; timeout/error paths unlink the temp file. Tests: spill content + tail alignment, small-output regression.
- **T5a frontend:** MessageList.tsx renders `edits` count, read ranges, bash timeout suffix; `tsc --noEmit` clean.
- **Verification run:** `ctest -R 'test_edit|test_write_file|test_read_file|test_bash|test_registry|test_safety'` — 7/7 passed under ASan+UBSan.

Remaining by design (`[defer]` in the phases above): T1d image reads, T3b fuzzy matching, T3c diff/patch payload, T5 per-path write serialization. T5b (PI_PARITY_REVIEW.md mapping + evidence archive) is the follow-up docs task.
