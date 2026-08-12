# Fix Verification Evidence — 2026-08-12 (TUI review sweep)

Every entry records the regression test, the fail-on-old-code evidence,
and the pass-on-new-code evidence, per AGENTS.md's verification
discipline. Environment: `nix develop`, gcc 15.2.0, ASan+UBSan+LSan on
(ENABLE_SANITIZERS=ON), Linux. Source: the TUI review of commit 762e267.

## T1 — tui_chat_wrap counted bytes, not display columns (CJK wrapped at 1/3 width, UTF-8 split mid-codepoint)
- Test: `test_wrap_counts_display_columns`, `test_wrap_combining_marks_are_zero_width`,
  `test_wrap_lone_wide_codepoint_never_loops`, `test_wrap_wide_word_moves_whole_word`
  (tests/tui/test_tui_chat.c)
- Fail (old byte-counting wrap): standalone harness against HEAD tui_chat.c —
  `cjk width4: got 3 lines, want 2`, `starts[1]=4 want 6` (split a 3-byte CJK
  codepoint mid-sequence), `combining width2: got 2 lines, want 1`,
  `wide-in-narrow: got 6 lines, want 2`, `wide-in-narrow: starts[1]=1 want 3`.
- Pass (new codepoint-based wrap): same harness exit 0; full suite 39/39 Checks.
- Why not wcwidth(3): returns -1 for non-ASCII in the C locale (verified with a
  probe binary), so a locale-independent width table is used instead.

## T2 — status/prompt-bar snprintf chains overflowed on truncation (tui.c)
- Affected: `render_status` (unbounded model-generated titles),
  `render_prompt_bar` (unbounded tool-argument JSON), `render_tools`.
- Fix: clamp `n` to `sizeof(line)-1` after every chain link before the next
  `snprintf(line + n, ...)`, so the chained writes stay in bounds.
- Test: none (notcurses rendering, not unit-testable); verified by
  sanitizer-clean full build + a trace of the truncation path
  (`n >= sizeof(line)` -> clamped, `sizeof(line) - n == 1`).

## T3 — round-trip event dropped when a modal was already open (worker hung, quit deadlocked in pthread_join)
- Scenario: ask_user modal -> Ctrl-C (answers question, opens confirm-quit) ->
  approval event arrives while the dialog is up -> old code returned without
  answering -> worker blocked forever in tui_event_wait_for_answer.
- Fix: `handle_event` answers NULL (deny) and lets the worker free the event.
- Test: no direct unit (needs agent); mitigated by T4's ring drain, which also
  answers any round-trip event still queued at destroy time.

## T4 — queued "run" jobs executed after quit (cancel flag reset per run; could deadlock the evs ring)
- tui_worker_submit accepts "run" while busy (type-ahead) and
  agent_run_streaming_new() resets cancel_requested at start, so
  tui_worker_destroy's cancel+join let queued runs execute in full while the
  UI thread was inside pthread_join and no longer draining the 1024-event
  ring — a long run blocked forever in tui_events_push.
- Fix: destroy drains the jobs ring before pushing TUI_EV_QUIT and drains the
  evs ring (answering, not freeing, round-trip events) before joining.

## T5 — /undo and /redo were functional no-ops in TUI mode
- run_tui never called registry_set_change_tracker(), so the write_file tool
  never snapshotted and the TUI's tracker always reported "Nothing to undo"
  (web mode wired it; main.c:613).
- Fix: main.c wires actx.ct the same way web mode does.

## T6 — tui_stream marker classification (both markers in one chunk; markers split across chunk deltas)
- Old tui_stream_split emitted the closing marker as think text when a chunk
  carried both markers, and could not resolve "<thi" + "nk>" at all.
- Fix: stateful TuiStreamClassifier (carry buffer, scratch merge, full scan
  loop, flush at end of run). API rewritten; worker owns one per run.
- Tests: test_both_markers_in_one_chunk, test_open_marker_split_across_chunks,
  test_close_marker_split_across_chunks, test_false_marker_prefix_is_emitted_once,
  test_alternating_markers_across_chunks, test_flush_emits_unresolved_carry,
  test_flush_inside_think_uses_think_kind, test_flush_nothing_pending
  (tests/tui/test_tui_stream.c) — 18/18 Checks green.

## T7 — free-then-dup lost the old pointer on allocation failure (job_load, slash_save)
- Fix: duplicate first, then swap; on OOM the previous session_id/title survives.
- Pattern per AGENTS.md multi-alloc commit rules; the worker's job_load is not
  directly unit-tested (needs an agent) but the fix is mechanical dup-then-free.

## T8 — tui_events_empty read count without synchronization (C data race)
- Fix: count is now _Atomic size_t (stdatomic); all ring ops use
  atomic_load/fetch_add/fetch_sub under the existing mutex; empty() reads it
  lock-free and stays documented as idle-detection-only.
- Tests: existing tui_events suite (30 Checks) still green; TSan not run (suite
  builds without it) — the atomic removes the UB by construction.

## T9 — wrap cache (render-chat O(n²) re-wrap per frame)
- Per-block cached line starts in TuiChatBlock, invalidated on text mutation
  (stream_append, tool_finish) and recomputed on width change; best-effort
  (malloc failure falls back to count-only wrapping).
- Tests: test_line_starts_cache_reuses_and_invalidates,
  test_render_lines_uses_cached_wrap (tests/tui/test_tui_chat.c).

## T10 — small fixes
- tui_app_create: fails cleanly instead of freopen(NULL) when no log path
  (HOME unset and log_path NULL).
- password/notice modal loops: wait for resize instead of rendering into NULL
  planes on <6-row terminals (new wait_for_terminal helper).
- PGDOWN scroll clamp now uses the renderer's wrap width (cols-3), not cols.
- Key bindings: NCKEY_DEL -> tui_input_delete, Ctrl-W -> tui_input_delete_word;
  editing resets the history walk (tui_input_reset_history_walk).
- RUN_DONE no longer ships the full aggregated content the UI ignored.
- TUI_EV_CANCEL_ACK removed (declared, never produced).
- Orphan comment removed; tui_events_push doc now states NULL text is stored
  as "" (matching the tested behavior).

## Evidence summary
- Full suite: 82/82 tests pass under ASan+UBSan (ENABLE_SANITIZERS=ON).
- No-TTY smoke: `./build/echo-ai --cli < /dev/null` exits 1 with the expected
  "requires an interactive terminal" error (no sanitizer reports).
- T1 fail-on-old-code: standalone harness compiled against HEAD tui_chat.c
  (5/5 assertions failed, including a mid-codepoint UTF-8 split); against the
  new code: exit 0.
