# Fix Verification Evidence — 2026-08-14 (TUI transparent-mode red background)

Environment: `nix develop`, ASan+UBSan+LSan on (ENABLE_SANITIZERS=ON),
Linux. Source: cancellation of an approval prompt (Esc) leaves the whole
chat pane with a red text background in `transparent = true` mode.

## Bug — error-role background leaked into every chat cell in transparent mode

- Symptom (user repro): cancel an approval request with Esc — the run
  ends with a "cancelled" error block in the chat and every text
  background in the chat pane turns red until the chat is cleared.
- Root cause: notcurses cells inherit the plane's channels at write time
  (`ncplane_put()` copies `n->channels` into the cell; verified in
  notcurses 3.0.17 src/lib/notcurses.c). `plane_color()` in
  `src/tui/tui.c` skipped `ncplane_set_bg_rgb8()` for non-status/error
  roles in transparent mode, so the plane's background channel was never
  reset. Painting a `TUI_BLOCK_ERROR` block (`TUI_ROLE_ERROR`, REVERSE
  style) sets the plane bg to opaque red `#f26d6d`; every cell written
  afterwards — all chat text on every subsequent frame, since
  `ncplane_erase()` does not reset `n->channels` either — inherited that
  red background. Masked in opaque mode, where every role repaints the
  background each line.
- Fix: new pure `tui_theme_plane_colors()` (tui_theme.h/.c) resolves
  role colors (REVERSE swap, DIM blend, status-bar bg rule) and reports
  whether the role paints an opaque background; `plane_color()` now
  calls `ncplane_set_bg_default()` for roles that do not, so a stale
  opaque background can never leak into later cells.

## Regression tests (tests/tui/test_tui_theme.c)

- `test_plane_colors_transparent_mode_resets_bg_for_plain_roles` — the
  direct regression claim: in transparent mode, BASE_FG/USER/ASSISTANT/
  TOOL_RESULT/BORDER resolve to a transparent background (opaque = 0),
  so the renderer resets the plane instead of reusing a stale bg.
- `test_plane_colors_error_role_reverses_to_opaque_red` — the error
  highlight keeps its opaque reversed red bg (fg=base_bg, bg=#f26d6d).
- `test_plane_colors_status_fg_keeps_opaque_accent_bg` — the status bar
  keeps its opaque accent field in transparent mode.
- `test_plane_colors_opaque_mode_always_paints_bg` — opaque mode is
  unchanged: every role paints its background.
- `test_plane_colors_think_dim_mixes_fg` — DIM roles blend fg toward bg
  as before.

Old-code behavior: the helper did not exist; the plain-role resolution
left the plane's background channel untouched, which is exactly the
leak. Fail evidence for the new claim on old code: the red-background
trace is a notcurses-rendering behavior (no unit coverage in CI — the
TUI requires a terminal), reproduced by the user on commit 905b40c with
`transparent = true`; the rendering path is traced in
docs/plans/TUI_MIGRATION_PLAN.md.

## Pass evidence

- `build/tests/tui/test_tui_theme`: 23/23 Checks pass (6 new).
- Full ctest: 84/84 pass, sanitizer-clean (ASan+UBSan+LSan).
- `make lint` on src/tui/tui.c, src/tui/tui_theme.c/.h,
  tests/tui/test_tui_theme.c: 4/4 clean (clang-tidy).

## Live verification requested

User repros with the rebuilt binary: Esc on the approval bar must no
longer turn the chat text backgrounds red.

## Follow-up (same day): denial indication restored

After the color fix the user reported the deny answer (Esc/n) left no
visible trace in the chat — the model carries on and textually
acknowledges the refusal, but the UI showed nothing. Previously the
aborted-run path surfaced a "cancelled" error block; the deny path never
did.

- Fix: `worker_approval` (src/tui/tui_worker.c) pushes a
  `TUI_EV_RUN_DONE` event with extra "cancelled" (empty text, so
  `handle_event` appends it as an error block instead of sealing a
  stream) whenever the approval answer is deny. The run itself still
  continues — the model is told the tool was refused — so the chat
  reads: tool-call text, "cancelled" block, model's acknowledgment.
- Reuses the exact channel the aborted-run path emits, so the wording
  and rendering match what the user saw before the color fix.
- Tests: full ctest 84/84 pass (ASan/UBSan/LSan), `make lint` clean.
  No unit test: `worker_approval` is static and needs a live agent to
  run (same coverage gap as T3 in the 2026-08-12 evidence doc);
  verified live by the user.
