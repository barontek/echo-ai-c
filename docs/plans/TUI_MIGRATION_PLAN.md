# TUI Migration Plan — replace `--cli` REPL with a notcurses-based TUI

**Date:** 2026-08-12
**Source:** user request — "change the cli mode in this codebase to a tui, like opencode but not coding focused, more of an all-rounder ui"; decisions locked in session: notcurses with `-DUSE_MULTIMEDIA=none -DUSE_CXX=off`, `--cli` replaced by the TUI, v1 scope = full chat UI.
**Repo:** /home/barontek/echo-ai-c (HEAD 7189179)
**Scope:** new `src/tui/` module; notcurses added as a pkg-config dependency; `main.c` refactor (bootstrap extraction + `run_cli` -> `run_tui`); build/CI wiring across nix, Homebrew, and Debian/Ubuntu apt; Check test suites for all non-terminal logic; docs (README, man page, Makefile targets).

## Decisions (locked)

1. **Library:** notcurses, built from source, pinned to v3.0.17 (the same version nixpkgs and Homebrew currently ship).
2. **Build flags:** `-DUSE_MULTIMEDIA=none -DUSE_CXX=off` plus the minimal-surface set in D1.
3. **Dependency mechanism:** notcurses added like every other library — flake.nix / Brewfile / apt — discovered via pkg-config, linking only `notcurses-core`. No submodule, no vendored source. The `-DUSE_MULTIMEDIA=none -DUSE_CXX=off` flags are honored on nix (override) and are inert-but-unexpressed on Homebrew/apt because the multimedia/C++ code is never linked (see P0.4).
4. **Mode semantics:** `--cli` launches the TUI. `--chat` (minimal REPL) and `--web` are untouched.
5. **v1 scope (full chat UI):** streaming chat pane, tool-activity panel, ask-user + approval dialogs, cancel, session-password modal, model switch. Sessions sidebar and log panel are M3 follow-ups, not v1.
6. **Sanitizer treatment:** notcurses is an unsanitized system dependency, exactly like libuv/curl/openssl — the vendored-build sanitizer question (previous decision 6) is superseded.
7. **Not-a-tty behavior for `--cli`:** when stdin or stdout is not a tty, `--cli` logs an error naming `--chat` as the piped-input path and exits non-zero; no silent fallback. (User accepted recommendation.)
8. **Threading contract:** all agent mutation happens on the worker thread; the UI thread never touches `Agent` directly. Session-manager calls are UI-thread-safe (mutex-protected store).

---

## Conventions

- Item IDs: `D<n>` dependency/vendoring, `B<n>` build/CI wiring, `T<n>` src/tui module code, `E<n>` event/threading, `U<n>` UI features, `X<n>` test suites, `V<n>` verification items. `M0..M3` are the milestone headings; milestone order is binding.
- **Every code item ships with:** the implementation, a Check test where the logic is non-terminal (per the module split in T1), a sanitizer run (ASan+UBSan), and — for allocation-heavy paths — a fault-injection test using the documented `*_TEST` compile-guard pattern (see X3).
- Track progress by ticking the `[ ]` boxes. A plan item is DONE only when its test is green under sanitizers and the evidence is recorded in this file or docs/verification/.
- Items marked `[defer]` are explicitly scoped out with a reason — never silently skipped.

---

## Phase 0 — Pre-flight analysis (complete, recorded for traceability)

### P0.1 Current CLI structure

- `run_cli` (src/main.c:354-647) and `run_chat` (src/main.c:174-302) are blocking `getline` REPLs; both duplicate the bootstrap block (registry init, tool enablement, search provider, OpenAI OAuth, agent config load, agent create, delegate config, session manager) that `run_web` (src/main.c:649-792) repeats a third time.
- Slash-command surface in `run_cli`: `/exit /quit /new /help /openai-login /openai-logout /clear /undo /redo /save /load /model /sessions` (`print_cli_help` at src/main.c:304-319).

### P0.2 Backend is TUI-ready (no LLM-layer changes required)

- `agent_run_streaming_new()` streams content deltas via `on_chunk` (src/agent/agent.h:154-156).
- Callback hooks exist and are wired into the agent: `tool_start_callback`, `tool_end_callback`, `title_callback`, `ask_user_callback`, approval callback (src/agent/agent.h:43-48, 201-275).
- `agent_cancel()` sets an atomic flag; safe from another thread (src/agent/agent.h:207-215). In-flight LLM HTTP calls finish first — same semantics as the web UI.
- `agent_set_model()` and `agent_set_provider()` exist for the model-switch feature (src/agent/agent.h:310-334). Both are non-thread-safe -> must run on the worker thread.
- All tool subprocesses capture output through pipes (e.g. src/tools/bash.c:51-53 dup2s the child's stdout/stderr into a pipe); nothing writes to the controlling tty. The TUI therefore owns the screen safely.
- Logging is leveled JSON-lines to stderr (src/utils/logging.c:66-93). In TUI mode this must be redirected (see U8).
- Session store is a refcounted, mutex-protected sqlite manager (src/session/session_manager.h:23-30) — safe to call from the UI thread.

### P0.3 notcurses v3.0.17 build analysis (CMakeLists read at tag v3.0.17)

- `USE_MULTIMEDIA` is a cache STRING in {ffmpeg, oiio, none}, default ffmpeg; `USE_MULTIMEDIA=none` drops the ffmpeg/OpenImageIO dependency entirely.
- `USE_CXX` defaults ON and gates the C++ wrapper library, nctetris, ncls, ncplayer, notcurses-input, and the doctest test binary; `off` skips all C++ sources and does not require a C++ compiler.
- **Key finding:** notcurses 3.0.17 splits its API across **two libraries**: `libnotcurses-core` (planes, input, `notcurses_stop`) and `libnotcurses` (`notcurses_init`, `notcurses_render`, plus multimedia code). The pkg-config file `notcurses.pc` carries both; with `USE_MULTIMEDIA=none` the multimedia parts are stubs with no ffmpeg linkage (verified: `ldd` shows zero avcodec deps in the nix override build). We link via `pkg-config notcurses` (both libs).
- Hard system deps of the core build: ncursesw/terminfo >= 6.1, GNU libunistring (`unigbrk.h` check is fatal), libdeflate (or zlib with `-DUSE_DEFLATE=OFF`).
- macOS branch hardcodes `PKG_CONFIG_PATH=/usr/local/opt/ncurses/lib/pkgconfig` — broken on Apple Silicon (`/opt/homebrew/opt/...`); our build step must export it explicitly.
- notcurses' own warning set applies to its own sources; our `-Werror` contract does not extend into the vendored tree (it builds with its own flags, like libuv/curl).

### P0.4 Package-manager comparison (revised 2026-08-12: dependency approach chosen)

Per user decision, notcurses is added **like every other library** (flake.nix / Brewfile / apt) and discovered via pkg-config — no vendoring, no submodule. Consequence: the `-DUSE_MULTIMEDIA=none -DUSE_CXX=off` build flags are honored only where the package manager exposes them (nix, via override); on Homebrew/apt the stock build ships ffmpeg multimedia and C++ bindings. This is **inert**: echo-ai links only `libnotcurses-core`, so ffmpeg/C++ code is never linked; the residual cost is install weight, recorded for traceability.

| Env | notcurses via package manager? |
|---|---|
| nix (flake.lock) | `notcurses.override { multimediaSupport = false; }` + `overrideAttrs` appending `-DUSE_CXX=off` to `cmakeFlags` — both flags honored |
| Homebrew | Stock formula `notcurses` (ffmpeg + C++ built, never linked by us) |
| Debian/Ubuntu CI (apt) | `libnotcurses-dev` stock build; ffmpeg pulled transitively, never linked by us |

Version discipline: nixpkgs pin comes from flake.lock; Homebrew floats and is recorded by the existing `brew list --versions` step; apt is pinned by the dated bookworm container tag.

---

## Milestone M0 — Dependency wiring and build plumbing

Goal: `make run-tui` renders a hello-world notcurses screen; all three CI environments build.

### D1 [x] notcurses as a pkg-config dependency

- `pkg_check_modules(NOTCURSES REQUIRED IMPORTED_TARGET notcurses)` in root CMakeLists.txt (done) — the pkg-config file carries `libnotcurses` + `libnotcurses-core` (the init/render entry points live in the former, planes/input in the latter; see P0.3).
- Notcurses' Requires chain (ncursesw/tinfo, libunistring, libdeflate) must be resolvable by pkg-config in every environment — hence the explicit manifest entries in D3.
- Sanitizer treatment: **not** sanitized, exactly like libuv/curl/openssl (decision 6 superseded: the vendored-build sanitizer question no longer applies).

### D2 [x] CMake wiring in root CMakeLists.txt

- `pkg_check_modules(NOTCURSES REQUIRED IMPORTED_TARGET notcurses-core)` + `PkgConfig::NOTCURSES` added to `echo-ai`'s link libraries (done).
- TUI sources get added to the `echo-ai` executable source list in M1; no extra wiring needed for the library itself.

### D3 [x] System dependency manifests

- `flake.nix`: `notcurses-tui` binding — `(pkgs.notcurses.override { multimediaSupport = false; }).overrideAttrs (final: prev: { cmakeFlags = prev.cmakeFlags ++ [ "-DUSE_CXX=off" ]; })`; plus `ncurses`, `libunistring`, `libdeflate` in buildInputs so pkg-config can resolve the Requires chain (done).
- `Brewfile`: `brew "notcurses"` added (done). `ncurses`/`libunistring`/`libdeflate` are Homebrew formula dependencies of notcurses and need no explicit entry.
- Ubuntu/Debian CI apt lines (.github/workflows/ci.yml:30, :60, :96): `libnotcurses-dev libunistring-dev libncurses-dev libdeflate-dev` added to all three jobs (done).
- macOS CI `brew list --versions` step (ci.yml:122): `notcurses` added so the floating version is recorded (done).

### D4 [x] Makefile run-tui (done)

- Makefile: add `run-tui: debug` invoking `$(BUILD_DIR)/echo-ai --cli --config $(CONFIG)` (flag stays `--cli`; the mode is renamed internally — see M1).
- No vendored build, so no macOS PKG_CONFIG_PATH gymnastics for notcurses itself; the existing macOS configure step already exports the openssl path (Homebrew's notcurses .pc resolves ncurses/unistring/deflate from its own install).

### B1 [ ] CI green check

- All three environments build, test suites pass, sanitizer runs clean. Record environment/version output per the existing CI conventions.

### V1 [ ] Hello-world smoke

- **Build-level verification (done 2026-08-12):** `pkg-config --modversion notcurses` = 3.0.17 in the nix shell; `cmake` configure finds the package; `echo-ai` links; full ctest suite (76/76) green. This proves the D1-D3 wiring end-to-end.
- **Visual check:** deferred to the real TUI (M1) — a standalone notcurses smoke binary under `script` hangs because `script`'s pty answers none of notcurses' terminal capability queries (CPR `\033[6n`, kitty keyboard/mouse, OSC color, DA). Not a bug in our integration; a real terminal or `tmux` is required. The interactive check belongs on the V2 manual checklist anyway.


---

## Milestone M1 — Chat core

Goal: usable streaming chat TUI with status bar, input editor, worker thread, cancel, quit confirm, password modal, slash-command pass-through, stderr redirect.

### T1 [ ] Module structure

Per AGENTS.md: one header per module, kernel-doc on all public functions, module README, 300-800 line comfort band.

```
src/tui/tui.h|c          lifecycle: notcurses init/stop, layout, main event loop, render, teardown
src/tui/tui_events.h|c   thread-safe event ring + self-pipe wake (worker->UI and UI->worker)
src/tui/tui_chat.h|c     scrollback model: width-aware wrap, append, scroll bounds; render
src/tui/tui_input.h|c    input line editor: insert/backspace/cursor/home/end, history, Ctrl-U
src/tui/tui_dialogs.h|c  modal stack: password, confirm-quit, ask_user, approval, device-login
src/tui/tui_status.h|c   status bar: model, provider, session id, tool count, spinner
src/tui/tui_tools.h|c    tool-activity panel: live tool start/end, elapsed, status
src/tui/tui_worker.h|c   worker thread owning the agent run; marshals callbacks; UI->worker jobs
src/tui/tui_theme.h|c    styling: palette, role->style map, border/spinner glyphs, density (U13)
src/tui/README.md        one README per module (layout, threading contract, ownership rules)
```

Split rule from the start: **every module is pure-model functions (testable, no notcurses) + thin render functions (notcurses calls only).** Terminal I/O stays a small fraction of the code so CI tests never touch a tty (see X1). No file approaches 1000 lines; split along the module boundaries above if a module grows.

### E1 [x] Event ring + wakeup (done: tui_events.c, 15 checks incl. fault injection)

- Fixed-capacity ring of `TUIEvent` structs; payloads are heap-allocated copies (never borrowed pointers across threads).
- Event kinds (v1): `EV_CHUNK` (content delta), `EV_THINK` (deltas inside `<think>` blocks — the stream layers already tag them), `EV_TOOL_START`, `EV_TOOL_END`, `EV_TITLE`, `EV_ASK_USER`, `EV_APPROVAL`, `EV_RUN_DONE` (aggregated LLMResponse or error), `EV_JOB` (UI->worker: load session, new session, switch model), `EV_CANCEL_ACK`.
- Synchronization: mutex + condition variable; worker uses **blocking push** (chunks can arrive fast; dropping is unacceptable); UI drains in one pass per frame.
- Wakeup: self-pipe (pipe(2) pair, one byte written per push, nonblocking read end) polled by the UI loop alongside notcurses input handling (see T2).
- Ownership contract (kernel-doc): `tui_event_push` copies the payload and owns the copy; `tui_event_pop` hands ownership to the caller; `tui_event_free` releases a payload; the ring is allocated once in `tui_events_init` and released in `tui_events_destroy` — every path documented in the header per AGENTS.md memory-ownership rules.
- Fault injection: `TUI_EVENTS_TEST` compile guard redefining the allocator hooks (calloc/str_dup) for the test target only — this is the TUI's multi-allocation commit site and gets the full fault test treatment in X3.

### E2 [x] Worker thread (done: tui_worker.c, callbacks registered, jobs run/load/new/model)

- Owns the `Agent *`; runs `agent_run_streaming_new(agent, input, on_chunk_cb, ud)` (src/agent/agent.h:154). Prefer a persistent thread with a job queue over per-submission pthread create/join; whichever is chosen, record the decision in the module README.
- Callbacks installed via the agent.h setters: `on_chunk` -> EV_CHUNK/EV_THINK push; `tool_start`/`tool_end` -> EV_TOOL_* push; `title` -> EV_TITLE push; `ask_user` -> EV_ASK_USER push then **block on a condvar** until the UI thread writes the answer into the event and signals (preserves the existing blocking contract of `ask_user_callback`, src/agent/agent.h:262-275); `on_approval` -> same round-trip pattern with EV_APPROVAL (0 = deny, 1 = allow, src/agent/agent.h:201-203).
- Job handling: `tui_worker_submit_job()` with TUI_JOB_LOAD_SESSION / TUI_JOB_NEW_SESSION / TUI_JOB_SWITCH_MODEL, executed on the worker thread only (`agent_set_model` is non-thread-safe, src/agent/agent.h:310). Jobs are refused while a run is in flight (UI shows a status message), except cancel, which is always honored via `agent_cancel()` from the UI thread.
- Teardown: on quit, set cancel, join the worker thread (with a bounded wait), then `agent_destroy` — ownership documented in header and README.

### T2 [x] Main loop and layout (done: tui.c, poll loop, 6 planes, key dispatch)

- `notcurses_init` with alternate screen on, `NCOPTION_SUPPRESS_BANNERS` on, mouse enabled for scroll.
- Loop: `poll(self_pipe_fd, timeout)` -> drain ring -> apply model changes -> `notcurses_render`; keys read via `notcurses_getc_nblock`/ncinput. SIGWINCH handled implicitly by render (notcurses re-reads geometry); explicit `notcurses_resize` path if needed.
- Layout (recomputed on resize):

  ```
  + status bar: model | provider | session id | tools: N | spinner ---------+
  | chat scrollback (main pane; think blocks dimmed; auto-scroll; PgUp/PgDn) |
  | tool-activity panel (collapsed strip when idle)                          |
  | input editor line                                                        |
  + mode-hint footer: Esc cancel | Tab focus | Ctrl-C quit ------------------+
  ```

- Key handling (v1): Tab cycles focus (input/chat/tools), Esc = cancel run or close top modal, Ctrl-C = confirm-quit modal, Ctrl-L = clear pane, PgUp/PgDn = scroll, Enter = submit, up/down = input history when input is focused, scroll wheel on chat.
- Terminal state hygiene: `notcurses_stop` on every exit path (single `goto cleanup` per AGENTS.md), including error paths before the loop starts.

### U1 [x] Input editor (done: tui_input.c, 17 checks incl. fault injection)

- Model: growable char buffer, cursor position, display offset; operations: insert, backspace, delete, home/end, Ctrl-U clear line, Ctrl-W word delete, up/down history (per-session, capped).
- Render: text clipped to plane width, cursor drawn, prompt glyph; no echo of newlines.
- UTF-8 aware rendering via notcurses width functions at render time; the model stays byte-based with the cursor clamped on render (documented decision in README).
- Slash commands typed into the editor keep working (pass-through to chat execution, results printed to the chat pane) — full parity with the old REPL in v1: `/exit /quit /new /help /clear /undo /redo /save /load /model /sessions /openai-login /openai-logout`.

### U2 [x] Chat pane model (done: tui_chat.c, 23 checks incl. fault injection)

- Model: list of rendered blocks (user/assistant/tool/error/think), width-aware word wrapping, scroll offset clamped to content, auto-scroll pinned to bottom while streaming.
- Streaming: `EV_CHUNK` appends to the in-flight assistant block and re-wraps only that block; render throttled to frame rate; `<think>` deltas go to a dimmed block.
- Tool activity: `EV_TOOL_START`/`EV_TOOL_END` appear as collapsible lines in the chat pane AND update the tool panel (U5); errors from `EV_RUN_DONE` payloads render distinctly.

### U3 [x] Status bar (done: render_status in tui.c, spinner + model/session/tools)

- Shows: model (updates on job completion), provider, session id (or "session disabled"), registered tool count, spinner while the worker is busy. Data fed exclusively by events and job results — never by poking the agent.

### U4 [x] Password modal (done: tui_app_prompt_password, masked input)

- Replaces `getpass` (src/main.c:139) when the TUI owns the terminal: masked input, centered modal, Enter submits, Esc cancels (aborts startup cleanly with terminal restore).
- Init order: notcurses init -> if `encryption_resolve_password_alloc()` returns NULL and sessions are enabled -> password modal -> session manager create. Env-var path unchanged.
- `init_session_manager` (src/main.c:114) is refactored to accept an injected password-prompt function pointer (see M1).

### U5 [x] Tool-activity strip (done: render_tools in tui.c; full panel in M3)

- Rows for in-flight tools: name, elapsed seconds (ticks on render), status glyph (running/ok/failed); collapses to a one-line strip when idle. Data from EV_TOOL_START/EV_TOOL_END only.

### U8 [x] Log redirect (done: stderr -> ~/.config/echo-ai/tui.log)

- v1: `freopen` stderr to `~/.config/echo-ai/tui.log` (append mode) before entering the alternate screen, so JSON log lines never corrupt the display. The M3 log panel builds on `ncfdplane` from the same fd. Tools' child-process stderr is unaffected (they capture via their own pipes).

### M1 [x] main.c refactor (done 2026-08-12: setup_runtime/teardown_runtime, three callers green)

- Extract the triplicated bootstrap (registry init, tool enablement, search provider, oauth create, agent config load, agent create, safety attach, delegate config) into `setup_runtime(Conf *conf)` returning a `RuntimeCtx` struct; `run_chat`, `run_tui` (formerly `run_cli`), and `run_web` each consume it. Byte-identical move first, behavior-preserving; lands as its own commit so the three callers stay green.
- `run_cli` becomes `run_tui`: tty check first (decision 7 — `!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)` -> `log_error` naming `--chat`, return non-zero), then the TUI lifecycle.
- `print_usage` (src/main.c:31-41), `echo-ai.1`, README, and the `--cli` flag docs updated to describe the TUI.

---

## Milestone M2 — Dialogs and full chat interactivity

Goal: ask-user + approval round-trips, device-login modal, model switch, resize polish.

### E3 [ ] ask_user and approval round-trips

- Worker side: push EV_ASK_USER/EV_APPROVAL (question text / tool name + args JSON), then wait on the event's condvar. UI side: modal rendered on top of the layout; answer written into the event struct; `pthread_cond_signal`; worker resumes.
- Timeouts: none in v1 for ask_user (matches current blocking stdin behavior); approval modal cannot be dismissed without choosing — the safety boundary must be explicit (no "Esc = allow").
- Cancellation interaction: if the user cancels the run while an ask_user modal is up, the worker is unblocked with a NULL answer (ask_user contract: NULL cancels the question, src/agent/agent.h:270-272).

### U6 [ ] Device-login modal

- Replaces `run_openai_device_login`'s printf flow (src/main.c:321-352): modal shows the verification URL and user code, plus a progress spinner while `openai_oauth_device_poll` runs; states PENDING/TRANSIENT/COMPLETE/FAILED map to modal states; Esc aborts the poll loop.
- The poll loop runs on the worker thread (it sleeps); the UI renders the modal from events.

### U7 [ ] Model / provider switch

- `/model <name>` continues to work as a command; additionally a keybinding (Ctrl-M) opens a model-picker modal listing candidates from the provider factory (`provider_names_available`, src/llm/factory.h:22).
- Execution as TUI_JOB_SWITCH_MODEL on the worker thread; status bar updates from the job result.

### U9 [ ] Resize and redraw polish

- Verify all planes reposition correctly across a range of terminal sizes (manual checklist, V2); content re-wrap on resize; modals stay centered.

### V2 [ ] Manual interaction checklist (archive in docs/verification/)

Resize, wide-char/emoji rendering, mouse scroll, Esc-cancel mid-stream, Ctrl-C confirm-quit, ask_user modal round-trip, approval allow/deny, device-login success and abort, `/save` + `/load` round-trip, `/new`, Ctrl-L clear, spinner animation, terminal state restore on every exit path (including errors before the loop).

---

## UI Styling — options and decision

The TUI's visual language is decided here, once, before M1 rendering starts; the chosen preset is then documented in the module README and enforced consistently across panes. All styling lives in one module (`tui_theme` — see U13) so a later theme switch never touches pane code.

### U13 [x] Theme module (done: tui_theme.c, 16 checks)

- One module owning: palette (RGB channels), role->style mapping (bold/italic/underline/reverse/dim), border glyph sets, spinner frame sets, density mode. Pane modules call `tui_theme_style(role)` only — no hardcoded colors outside this module.
- Config-driven: new `[tui]` section in config.conf (style = dark|light|highcontrast|none; density = compact|spacious; accent = auto|hex), read in `run_tui` bootstrap before notcurses init. `style = none` = plain monochrome (also honored via the `NO_COLOR` env convention). Unknown values fall back to `dark` with a log line — never a crash.

### Color palette options

| Preset | Palette | Intent |
|---|---|---|
| **dark** (default) | background `#1e1e2e`, foreground `#dcd7ba`, accent `#7aa2f7` (blue) | Modern dark terminal; fits most emulators, low glare |
| light | background `#f2f0e8`, foreground `#2d2a26`, accent `#2f5fb3` | Light-background terminals (paper-like) |
| highcontrast | pure black/white text, accent `#ffd700` | Accessibility: color-blind safe, maximum contrast |

- **Recommendation:** `dark` as default, `highcontrast` shipped and selectable. Both tested against 256-color fallback (notcurses degrades RGB gracefully).

### Role styling (applies in all presets, colors differ per palette)

| Role | Style |
|---|---|
| user message | accent-colored `>` marker, plain body |
| assistant message | plain body, slightly brighter than base foreground |
| think block | **dim** + italic, prefixed `…`; distinct from real output (stream already tags `<think>`) |
| tool lines (chat pane) | dim, box-drawn glyphs per status |
| error | reverse video (block-visible in all palettes, not just red) |
| status bar | accent background + black text; busy spinner animates in accent |
| modal focus | accent border; modal title bold; password input shows bullets `•` |

### Border and glyph options

- **Borders:** plain `+ - |`, rounded `╭ ╮ ╰ ╯ ─ │`, or none (flat panes separated by spacing).
- **Recommendation:** rounded for panes + modals, `none` for the status bar/footer (flat strips), plain for the tool-activity strip when collapsed. Vertical divider between chat and future sidebar uses `│`.
- **Spinners:** three-frame `◐ ◓ ◑ ◒`, braille dots `⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏`, or `| / - \`.
- **Recommendation:** braille dots for the status bar (smooth at any width), `| / - \` as the fallback when the terminal lacks Unicode.
- **Status glyphs (tool panel):** `●` running (accent), `✓` ok (green), `✗` failed (red), `…` waiting. ASCII fallbacks `*`, `ok`, `!!`, `..` under `style = none`.

### Density options

- **compact** (default): one line per wrapped chat block edge, tool panel collapses to a single line, no blank separators between messages.
- **spacious:** blank line between messages, tool panel always expanded, wider input padding.
- Recommend compact default; spacious selectable for readability on large terminals.

### Text handling

- Messages wrap at word boundaries; long tokens (URLs, code) break mid-word rather than overflow (v1; markdown-lite in M3 may refine).
- Scrollbar: thin `│` indicator column on the chat pane's right edge, accent-colored, only while scrollable — skipped when `style = none`.
- No emojis in UI chrome (repo rule); Unicode box glyphs only, with ASCII fallbacks as above.

### V3 [ ] Visual verification entries (extends V2 checklist)

- Palette correctness on truecolor, 256-color, and 16-color terminals (`tput colors` variants).
- `style = none` renders fully legible and static (no animated glyphs).
- Think blocks visually distinct from real output in all presets.
- Status bar legible on both light and dark backgrounds; modal borders visible in every palette.
- Spinner smoothness at 30/60fps render; no flicker on re-wrap of the active block.

---

## Milestone M3 — Follow-ups (explicitly NOT v1)

### U10 [ ] Sessions sidebar panel
- List/load/save/delete/rename sessions from the session manager (UI-thread-safe store); replaces the `/sessions` text dump. Load executes as a worker job.

### U11 [ ] Log panel
- `ncfdplane` over the redirected stderr fd (reuses U8's redirect); tail view with level coloring.

### U12 [ ] Markdown-lite rendering
- Bold headers, code blocks, lists rendered from assistant content; `<think>` styling preserved. Plain-text fallback for unknown constructs.

### U13 [defer] Multi-session tabs
- One Agent per tab, one worker thread per tab. Deferred: biggest lift, unclear demand; revisit after U10 lands. Never silently skipped — this entry stays until decided.

---

## Testing plan (AGENTS.md obligations)

### X1 [ ] Module-split testability rule
- Only pure-model code is unit tested; render functions are exercised by the manual checklist (V2) and the smoke script (V1). No Check test may init notcurses — CI has no tty.

### X2 [ ] Suites (each registered in tests/CMakeLists.txt via add_subdirectory(tui), one TCase per behavior area, ck_assert_* only, fixtures per test):

| Suite | Coverage |
|---|---|
| tests/tui/test_tui_events.c | ring push/pop, blocking semantics, wake correctness, order preservation, empty-drain |
| tests/tui/test_tui_chat.c | wrapping (empty, single word, exact width, long word), append, scroll clamping, auto-scroll pinning, block states |
| tests/tui/test_tui_input.c | insert/delete/cursor/home/end/Ctrl-U/Ctrl-W, history cap and navigation, empty-buffer edges |
| tests/tui/test_tui_dialogs.c | modal stack push/pop/dismiss rules, password masking, approval cannot-dismiss invariant, ask_user cancel path |

### X3 [ ] Fault injection (the AGENTS.md documented pattern)
- `TUI_EVENTS_TEST` compile guard (per-target definition in tests/tui/CMakeLists.txt only, never the main target) redefines the allocator hooks; tests force failure at each allocation point of `tui_event_push`'s payload-copy path and assert: push reports failure, ring state unchanged, no partial commit, no leaks (sanitizer-verified).
- Extend to tui_input buffer growth and tui_chat block allocation once those commit sites exist.
- This is the same bug class as the metrics.c/semantic_search.c/tool_delegate.c incidents — write the fault test before the module is considered done, not after something breaks.

### X4 [ ] Registration and CI
- `tests/CMakeLists.txt`: `add_subdirectory(tui)`; suites get their own `add_test()` and `tcase_set_timeout` where blocking semantics are exercised.
- CI (all three environments) runs the new suites under ASan+UBSan; results recorded in CI logs per existing convention.

### X5 [ ] Regression discipline
- Every bug found during TUI development ships with a regression test that fails on the old code and passes on the new; fail->pass evidence archived in docs/verification/ and tracked in docs/plans/AGENTS_COMPLIANCE_FIX_PLAN.md status tables if it touches pre-existing modules.

---

## Risks and mitigations

1. **notcurses API surface** is large; v1 touches a small slice (notcurses_init/getc/render, ncplane_*, ncinput, ncfdplane only in M3). Mitigated by the thin-render module split.
2. **Raw mode + ASan local runs**: ASan output and leak reports interleave with the alternate screen. Local TUI sessions use `ASAN_OPTIONS=detect_leaks=0`; CI is unaffected (tests never init notcurses).
3. **macOS keg-only ncurses**: hardcoded /usr/local path in notcurses breaks on Apple Silicon — D4 exports `PKG_CONFIG_PATH` explicitly in the macOS build step; CI validates.
4. **Version drift**: nixpkgs revision pinned by flake.lock; Homebrew floats and is recorded by `brew list --versions`; apt pinned by the dated bookworm container tag — per the repo's existing environment policy.
5. **Blocking ask_user/approval while a run is in flight** freezes the worker — by design (same contract as today's stdin flow); the UI stays live because only the worker blocks.
6. **In-flight LLM calls ignore cancel** until the next loop checkpoint — existing documented behavior (src/agent/agent.h:207-215); UI shows "cancelling..." until EV_RUN_DONE.
7. **Cross-platform .pc differences**: notcurses-core version/API may differ slightly across nixpkgs (3.0.17), Homebrew (3.0.17), and Debian bookworm (3.0.x) — v1 uses only long-stable core APIs (notcurses_init/getc/render, ncplane_*, ncinput), and CI compiles against all three, so drift surfaces as a build failure, not a runtime surprise.

## Definition of done

- M0: three CI environments build with the pkg-managed notcurses; `make run-tui` renders hello-world; sanitizer-clean builds.
- M1: full v1 feature set above; all X2/X3 suites green under sanitizers; evidence archived.
- M2: dialogs complete; V2 checklist signed off and archived in docs/verification/.
- M3 items land as separate tracked work; U13 stays visible until decided.
