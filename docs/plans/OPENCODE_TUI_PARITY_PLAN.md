# OpenCode TUI Parity Plan — bring echo-ai's TUI to opencode feature parity

**Date:** 2026-08-17
**Source:** docs/reviews/OPENCODE_TUI_REVIEW.md — exhaustive inventory of opencode's TUI (`anomalyco/opencode` @ `a97fec8`, `packages/tui`). Review sections are cited below as `[R#]`.
**Repo:** /home/barontek/echo-ai-c
**Scope:** `src/tui/` (notcurses-based chat TUI, 15 files, ~8.7k LOC) plus the minimal agent/session/tools surface it needs. The web frontend, server routes, and LLM providers are **out of scope** — this plan is TUI-only.
**Predecessor:** docs/plans/TUI_MIGRATION_PLAN.md (baseline TUI build, M0–M3) is complete; this plan builds on top of it. Items that were deferred there and are still relevant here are re-opened with a reason.

## Goal

Make echo-ai's TUI behave and look like opencode's: same interaction model (leader-key chords, command palette, session management, prompt composer with autocomplete, rich permission/question prompts, sidebar, diff viewer) mapped onto echo-ai's notcurses + config.conf + worker-thread architecture. "Exactly like opencode" in interaction semantics; the rendering library and config file format stay echo-ai's (notcurses, `config.conf`), because swapping those is out of scope.

## Principles

1. **Parity where it fits; safety never regresses.** The approval modal (src/tui/tui_dialogs.h `TUI_MODAL_APPROVAL`) must stay explicit — no "Esc = allow" ever, matching opencode's reject-is-explicit rule `[R2.7]`. Safety checks in `src/safety/` are untouched.
2. **The review is the contract.** Every item cites the opencode behavior it copies (`[R#]`); where echo-ai deliberately deviates, the deviation is stated inline with a reason.
3. **Keep the model/render split.** Pure-model logic (keymap table, command registry, session list state, diff model, autocomplete ranking) is unit-tested with Check and gets fault-injection tests at multi-allocation commit sites (AGENTS.md pattern). Notcurses calls stay in thin render functions, never tested in CI (no tty).
4. **Config lives in `config.conf`**, not a new `tui.json`. New `[tui]` keys extend the existing `style/density/accent/transparent` block. Unknown values fall back with a log line — never a crash (existing convention).
5. **Worker-thread contract is unchanged.** All agent mutation stays on the worker; the UI thread drives models/stores. New worker jobs (compact, branch-at-message) follow the existing `tui_worker_submit` + event pattern.
6. Items marked `[defer]` are scoped out with a reason — never silently skipped.

---

## Phase 0 — Gap analysis (echo-ai today vs opencode TUI)

Legend: ✅ present, 🔶 partial, ❌ missing. `[R#]` = review section.

### Input & prompt composer

| opencode feature `[R]` | echo-ai status | Current state |
|---|---|---|
| Leader-key chords (`<leader>l`, `<leader>g`, …) `[R2]` | ❌ | Input handling is a flat `if` chain in `handle_key` (src/tui/tui.c:2620); no prefix/chord engine |
| Command palette `ctrl+p` `[R2.1][R3]` | 🔶 | A fixed menu (`open_menu`, tui.c:1662) opened by Ctrl-M/Ctrl-P; no search, no categories, no keybind display |
| Configurable keybinds `[R14]` | ❌ | Keys are hardcoded; `[tui]` config has appearance keys only |
| Slash-command registry + discovery `[R3]` | 🔶 | Hardcoded `handle_command` chain (tui.c:2373); no registry, no per-command help/category, no discovery in autocomplete |
| `@` file / `/` command autocomplete `[R5]` | ❌ | No autocomplete; input is a plain editor |
| Persistent prompt history + stash + frecency `[R5]` | 🔶 | In-memory history only (`tui_input`); no persistence, no stash, no frecency |
| Shell mode (`!cmd`) `[R3]` | ❌ | Not present (TUI has no shell-execution path) |
| External editor (`$VISUAL/$EDITOR`) `[R5][R17]` | ❌ | `/edit <n> <text>` edits a user message inline; no editor launch |
| Multiline input | ✅ | Shift+Enter inserts newline (tui.c:2694-2701) |
| Paste handling / bracketed paste | 🔶 | Raw input; no CRLF normalization or paste summarization |

### Session & navigation

| opencode feature `[R]` | echo-ai status | Current state |
|---|---|---|
| Session list dialog: search, pin/unpin, delete, rename, quick-switch `<leader>1-9` `[R4.2][R2.2]` | 🔶 | Session picker (`/sessions`, `open_session_picker`) filters and switches; no pin, no quick-switch, no dialog-side rename/delete |
| Home view with session list + tips `[R8]` | ❌ | TUI boots straight into chat |
| Timeline + fork-from-timeline `[R4.3]` | 🔶 | `/branch` + `/regen` exist; no message-level timeline UI |
| Compact/summarize `[R3]` | ❌ | `agent_perform_summarization` exists (src/agent/agent_summarize.h) but is not exposed to the TUI |
| Share / unshare `[R3]` | ❌ | No share backend |
| Export transcript with options dialog `[R4.1]` | 🔶 | `/export <id> [path]`; no options (thinking/tool-details) |
| Copy transcript / message to clipboard `[R2.2][R4.3]` | ✅ | `/copy` via OSC 52 (tui.c:2139) |

### Views & panels

| opencode feature `[R]` | echo-ai status | Current state |
|---|---|---|
| Sidebar: context tokens/cost, modified files, todos, MCP, LSP `[R7]` | ❌ | No sidebar; single chat pane |
| Diff viewer (file tree, hunks, mark reviewed) `[R9]` | ❌ | `change_tracker` undo/redo exists but has no visual diff |
| Status dialog + debug dialog `[R4.2]` | ❌ | Status bar shows model/provider/session/tools; no /status or /debug dialogs |
| Scrollbar + scroll acceleration `[R2.2]` | 🔶 | PgUp/PgDn + wheel scroll; no scrollbar, no acceleration |
| Timestamps toggle, conceal, thinking toggle `[R2.2]` | 🔶 | Think blocks render dimmed; no toggles, no timestamps, no conceal |

### Safety & questions

| opencode feature `[R]` | echo-ai status | Current state |
|---|---|---|
| Permission prompt: Allow once / Always / Reject + pattern list + reject-with-message `[R6.4]` | 🔶 | Approval modal is a binary y/n (`TUI_MODAL_APPROVAL`); no always-with-patterns, no reject message |
| Question prompt: tabs, numbered options, multi-select, custom answer, review `[R6.5]` | 🔶 | `TUI_MODAL_ASK_USER` is a single free-text answer |
| Per-permission rendered bodies (edit diff, bash command, webfetch URL) `[R6.4]` | 🔶 | Approval shows tool name + args JSON; no rich per-type bodies |
| Auto-approve mode (`--auto`/`--yolo`) `[R16]` | 🔶 | Config gating exists in the safety layer; no TUI flag |

### Model / provider / theming

| opencode feature `[R]` | echo-ai status | Current state |
|---|---|---|
| Model dialog (favorites/recent/popular) + cycling (`f2`/`shift+f2`) `[R2.1][R4.2]` | 🔶 | Menu → provider/model picker; no favorites/recent/cycling keys |
| Variant cycling (`ctrl+t`) `[R2.1]` | 🔶 | Reasoning-effort picker exists; no cycling key |
| Theme list dialog with live preview + many presets `[R13]` | 🔶 | 4 styles (dark/light/highcontrast/none), `/theme` command; no picker preview |
| Notifications / attention / sound `[R11]` | ❌ | No OS notifications or sound |
| Tips on home `[R12]` | ❌ | No tips |

### Architecture

| opencode feature `[R]` | echo-ai status | Current state |
|---|---|---|
| Plugin slot system `[R10]` | ❌ | No plugin system |
| Which-key panel `[R2.8]` | ❌ | No |
| Toasts `[R4.4]` | 🔶 | Inline `chat_notice`; no transient toast layer |
| Help dialog with keybind table `[R4.1]` | 🔶 | `/help` static text (tui.c:2057) |
| Editor context (SSE editor link, file+selection chip) `[R5][R17]` | ❌ | No editor integration |
| Subagent navigation (parent/child) `[R2.2][R6.6]` | ❌ | `delegate` tool exists for the LLM but subagent sessions are not surfaced in the TUI |
| Agent switching (tab/shift+tab) `[R2.1]` | ❌ | Single configured agent; `agent_factory` only for `/new` |

---

## Known issues (open)

- **BUG (2026-08-17, reported):** pressing Enter on **"Command menu"** in the command palette (`app.menu` → `open_menu`) makes the TUI exit. Not yet diagnosed. Symptom points at the `PICKER_COMMAND` close-first commit path or a crash during the palette→menu modal handoff (values/headers alignment, plane lifecycle, or a double `close_modal`). Verify under ASan before marking fixed; the fail→pass reproduction must be captured first.

## Progress (2026-08-17)

**Completed and verified (ASan+UBSan, full ctest green):**

- **Phase A1** `[x]` — `src/tui/tui_keys.c/h`, `tests/tui/test_tui_keys.c` (19 checks). Key-string parser, binding table, dispatch, leader chord engine, config overrides.
- **Phase A2** `[x]` — leader-key engine with timeout, chord resolution, fall-through on miss; footer leader hint.
- **Phase A3** `[x]` — `[tui] leader`, `leader_timeout`, `keymap_<command>` overrides in config.conf.example.
- **Phase A4** `[x]` — `/keybinds` read-only picker.
- **Phase B1** `[x]` — `src/tui/tui_command.c/h`, `tests/tui/test_tui_command.c` (5 checks). All slash commands + keymap commands migrated to one registry.
- **Phase B2** `[x]` — command palette on `ctrl+p` (`PICKER_COMMAND`, picker values support in `tui_dialogs`). Now opencode-ordered: a **Suggested** head, then commands grouped under category headers (`tui_modal_picker_set_headers` — non-selectable rows that navigation/filtering/commit skip; 3 tests).
- **Phase B3** `[x]` — `/help` command (text) + `/keybinds` picker; palette shows keybindings per command.
- **Phase B4** `[x]` — `/compact` (worker `compact` job → `agent_perform_summarization`, 2 tests), `/status`, `/debug` dialogs.
- **Fixes in the same pass:** palette commands that open a dialog were instantly closed by the ANSWERED `close_modal` (PICKER_COMMAND/PICKER_MENU now close-first then return replaced).
- **Phase C2** `[x]` — `src/tui/tui_prompt_store.c/h` (10 checks): persistent `prompt-history.jsonl` + `prompt-stash.jsonl`; history seeded on startup, appended on submit; `/stash`, `/stash-pop`, `/stash-list` picker; `tui_input_seed_history` (1 check).
- **Phase C3** `[x]` — shell mode: leading `!` submits through a new worker `shell` job → `registry_get("bash")` (safety checks + timeout apply), output rendered as a `shell` tool block; `$` prompt glyph + footer hint; 2 worker tests.
- **Phase D1 (partial)** `[x]` — `src/tui/tui_session_store.c/h` (7 checks): persistent session pins (`sessions.json`), pin toggle, and quick-switch slot resolution (pins first, then recency). Wired: `session.pin` (`/pin`, `<leader>p`) and `session.quick_switch.1..9` (`<leader>1-9`) load via the worker. Not yet done: pin badges/ordering inside the session picker dialog itself (open_session_picker still lists flat).
- **Phase C4** `[x]` — `/editor` (`<leader>e`): opens the draft in `$VISUAL`/`$EDITOR`; TUI suspended (terminal restored, SIGINT defaulted), text read back on return.
- **Phase D3** `[x]` — `/timeline` (`<leader>g`) picker of user messages, scrolls to the selected block; `/fork` (`<leader>f`) re-answers from the selected message via the existing regen/branch machinery.
- **Phase E1 (core)** `[x]` — approval prompt is now a three-option prompt: **Allow once** (Enter/y) / **Always** (2) / **Reject** (Esc/n), navigable with h/l/arrows/tab/1-3. "Always" persists a runtime rule via the new `safety_allow_tool_always()` (removes the tool from `require_approval_for`, until restart). 7 new dialogs tests + 1 safety test. Not yet done: reject-with-message (needs the agent approval callback to carry a message) and per-tool rich bodies (diff preview) — both documented follow-ups.
- **Phase C1 (slash)** `[x]` — `src/tui/tui_autocomplete.c/h` (5 checks): Tab completes typed slash commands (unique match → `/<cmd> `, multiple → longest common prefix). `@`-file completion deferred (needs a file index + panel).
- **Phase C5** `[x]` — `tui_input_insert` normalizes CRLF/lone CR to LF (2 tests), so pasted Windows text is clean.
- **Phase E2 (core)** `[x]` — `ask_user` tool accepts an optional `options` array; the question is rendered with numbered choices and a bare-number answer resolves to the chosen option (4 tests). Full tabs/review UI deferred (the modal remains a single-text answer over the numbered body).
- **Phase H1** `[x]` — `src/tui/tui_model_store.c/h` (4 checks): persistent recent/favorite model lists (`model-store.json`). Model picker leads with ★ favorites / ↺ recent; `f2`/`shift+f2` cycle recent models; setting a model records it as recent.
- **Phase F (lean)** `[x]` — sidebar (`<leader>b`): a right panel showing session facts (id/model/provider/tools) and an honest "not available" section — echo-ai's core exposes no MCP/LSP/todo/context-token sources, and `change_tracker` tracks single-file undo history, not a multi-file list. The full opencode-style sidebar panels are not applicable to this architecture.

**Deferred with reasons (stay visible until decided):**

- **G — diff viewer** — echo-ai's `change_tracker` stores single-file undo snapshots, not diff text; the only diff surface today is the inline tool-diff rendering in chat (`put_diff_line`). A fullscreen viewer needs a diff store in the core first.
- **D2 — home view** — a config-gated landing view needs a second layout mode; the direct-to-chat flow already covers session switching via `<leader>l`/quick-switch. Low value vs. cost.
- **D4 — export options dialog** — the current `/export` writes the raw session JSON; a markdown transcript with thinking/tool-details options needs a markdown exporter in the session module first.
- **H2 — theme live-preview dialog + extra presets** — the `/theme` picker exists; a live-preview dialog and 6+ new palettes are cosmetic and can layer on `tui_theme.c` later.
- **H3 — status bar cost/context polish** — context-token/cost data is owned by the agent (not exposed to the TUI); the status bar already shows model/provider/session/tools.

### Structural refactor (2026-08-17, alongside Phases A–C3)

`src/tui/tui.c` had grown to 3916 lines (over AGENTS.md's 1000-line split signal). Split into three responsibility-scoped files plus a private shared header:

| File | Lines | Owns |
|---|---|---|
| `src/tui/tui.c` | 1171 | app shell: lifecycle, signals, layout, main loop, key handling, event dispatch |
| `src/tui/tui_render.c` | 988 | all notcurses drawing (chat/status/tools/input/footer/modal panes) |
| `src/tui/tui_cmd.c` | 973 | command registry, slash handlers, command dispatch, registry init |
| `src/tui/tui_picker.c` | 742 | picker/dialog UI: provider/model/menu/theme/session pickers, palette, stash, /status /debug |
| `src/tui/tui_internal.h` | 167 | shared TuiApp struct + cross-file helper declarations (internal only) |

All TUI sources are within AGENTS.md's 300–1000-line comfort band (`tui.c` sits at 1171 as the app shell; further splitting there is possible but the remaining content is one cohesive lifecycle unit). Full suite (92) green under ASan+UBSan after the split.

---

## Phase A — Keybinding foundation

Goal: a data-driven keymap so every later phase registers bindings instead of extending `handle_key`'s if-chain, and so users can rebind keys in `config.conf`. This is the structural prerequisite for the palette, which-key, and the leader chord.

### A1 [ ] Central keymap table (`tui_keys.h|c`)
- New module: `TuiKeybind { const char *name; const char *keys; const char *category; const char *desc; keybind_fn fn; void *ud; }` plus a registration/binding layer. Pure model, unit-tested.
- Name strings mirror opencode's command names (`session.list`, `model.cycle`, `app.exit`, …) so the review's command vocabulary `[R2]` carries over and later palette/help output matches.
- Dispatch: a single `tui_keymap_dispatch(app, id, ni)` replaces the hardcoded branches in `handle_key` (tui.c:2620). Key sequences are `"ctrl+x"`, `"enter"`, `"esc"`, `"f2"`, `<leader>m` etc., parsed by a small key-string parser (same shape as opencode's `keybind.ts` `[R14]`).
- **Tests** (`tests/tui/test_tui_keys.c`): parse table, alias expansion (`enter→return`), exact-match precedence, override precedence, `false`/`none` disable, unknown-key fallback to printable insertion.
- **Verify:** existing TUI key behavior unchanged (every current key has an equivalent entry); full TUI suites green under ASan+UBSan.

### A2 [ ] Leader-key engine
- Implement opencode's leader chord `[R2 intro]`: a `<leader>` token (default `ctrl+x`) arms a pending sequence; within `leader_timeout` (default 2000 ms, configurable) the next key resolves `<leader>X`. Esc cancels the pending sequence; backspace pops it. Timed leader matches `keymap.tsx:214-232` `[R14]`.
- Map current single-key actions to chords where opencode does: `<leader>l` sessions, `<leader>n` new, `<leader>m` models, `<leader>t` themes, `<leader>g` timeline, `<leader>u`/`<leader>r` undo/redo, `<leader>c` compact, `<leader>1..9` quick switch. Existing keys (`ctrl+m` menu, `ctrl+c` quit, `esc` cancel) stay.
- **Tests:** chord resolution, timeout expiry, cancel-on-esc, quick-switch slot resolution.
- **Verify:** manual interaction with tmux + a leader chord; no renderer involvement in the pure engine.

### A3 [ ] Configurable keybinds (`[tui]` config)
- New `[tui]` keys: `keymap = { name = "keys", value = "ctrl+m" }` style per-binding overrides (parseable list of `command = key` pairs), `leader_timeout`, `leader = ctrl+x`. Unknown key names log and are skipped (existing convention, config.conf).
- **Tests:** config parse/validation in the existing `test_config` suite; override applied in `test_tui_keys`.

### A4 [ ] Keymap dump + `/keybinds` command
- `tui_keymap_describe()` renders the full table (name / key / category / description) for the help dialog (B3) and a `/keybinds` command that prints it into the chat pane (like `/help` today).

---

## Phase B — Command registry, palette, help, and status dialogs

### B1 [ ] Command registry (`tui_command.h|c`)
- Refactor `handle_command` (tui.c:2373) into a registry: `TuiCommand { name, title, desc, category, slash_name, slash_aliases[], suggested, enabled, fn }`. Slash dispatch becomes a lookup (`/name` → command) with alias support (`/mo`→models, `/summarize`→compact) matching `[R3]`.
- Every existing slash command is migrated: `/help /new /undo /redo /sessions /save /load /model /provider /effort /delete /rename /export /change-password /openai-login /openai-logout /menu /lock /unlock /theme /copy /edit /regen /branch /clear`.
- New commands added in later phases register here: `/compact` (B4), `/status` (B4), `/debug` (B4), `/keybinds` (A4), `/skills`-style pickers when they exist.
- **Tests** (`tests/tui/test_tui_command.c`): registration, alias resolution, unknown-command fallback ("Unknown command. Try /help."), category/suggested flags.

### B2 [ ] Command palette (`ctrl+p`)
- A `TUI_MODAL_PICKER` populated from the registry (`[R2.1][R3]`), title "Commands", filter matches name/title/category/aliases (the picker already type-to-filters — tui_dialogs.h). Each row shows its keybinding when bound; a "Suggested" header lists `suggested` commands when the filter is empty `[R3]`. Selecting dispatches.
- Replaces the fixed `open_menu` (tui.c:1662); Ctrl-M remains a direct alias for the palette so muscle memory is preserved.
- **Tests:** picker data assembly (filtering, suggested ordering) in `test_tui_command`.

### B3 [ ] Help dialog with keybind table
- `/help` and `?` (in chat focus) open a scrollable modal listing commands grouped by category with their keys, sourced from A4's describe output — replacing the static `slash_help` text `[R4.1]`.

### B4 [ ] `/compact`, `/status`, `/debug`
- **`/compact`** — new worker job `compact` calling `agent_perform_summarization` (src/agent/agent_summarize.h); result echoed as a system notice. Slash aliases `summarize` `[R3]`. **Tests:** worker job dispatch + busy rejection; summarization path is already agent-side tested.
- **`/status`** — dialog listing MCP/tools/formatter status where data exists (tool registry enablement, provider), mirroring `DialogStatus` `[R4.2]`; honest "not applicable" rows where echo-ai has no LSP/plugins.
- **`/debug`** — dialog with version, date, OS, terminal, session id, model/provider, copy-to-clipboard button (OSC 52 path exists), mirroring `DialogDebug` `[R4.2]`.

---

## Phase C — Prompt composer

### C1 [ ] Autocomplete (`@` files, `/` commands)
- New `tui_autocomplete.h|c`: pure ranking/filtering fed by (a) a file index built from `glob_tool`/directory walk (capped, workspace-local), (b) the command registry for `/` names, (c) tool names. Fuzzy match + prefix bonus, top-10 panel drawn above the input `[R5]`.
- Keys while the panel is open: `up/down` move, `tab` completes, `esc` hides, `enter` selects (mirrors `[R2.3]`). `@` triggers file/mention; `/` triggers commands.
- **Tests:** trigger detection, filtering/ranking, insertion into the input buffer, hide conditions. File search itself reuses existing tested tools; no new I/O path in the model.

### C2 [ ] Persistent prompt history + stash
- Persist submitted prompts to `<state>/prompt-history.jsonl` (max 50, consecutive-dedup) and add a stash (`/stash`, `/stash pop`, `/stash list` picker) mirroring `[R5]`. Reuse the atomic JSONL write pattern from `utils`; state dir follows `~/.config/echo-ai/`.
- History still walks with up/down; persistence only adds a load-on-start + append-on-submit. **Tests** (existing `test_tui_input` extended + new `test_tui_prompt.c`): file round-trip, dedup, cap, corrupt-line recovery (self-heal).

### C3 [ ] Shell mode (`!cmd`)
- Leading `!` at cursor 0 switches the input to shell mode (status hint `SHELL`); submit sends the command to a new worker job `shell` that runs it via the existing bash tool path and streams output into the chat as a tool block `[R3][R2.3]`. Esc at position 0 exits shell mode.
- **Tests:** mode transitions; worker job dispatch. Execution path reuses `tools/bash.c` (already tested).

### C4 [ ] External editor (`$VISUAL/$EDITOR`)
- `prompt.editor` (leader `<leader>e`, slash `/editor`): write the current draft to a temp `.md` file, suspend the renderer, spawn `$VISUAL || $EDITOR` (fork+execvp pattern from `tools/bash.c`), read back, resume, replace the buffer `[R5][R17]`. Esc-cancel path restores the draft.
- **Tests:** temp-file round-trip with a scripted editor (env-overridden `$EDITOR`); renderer suspend/resume is manual (V-check).

### C5 [ ] Paste handling
- Bracketed-paste aware input: normalize CRLF→LF; collapse huge pastes to a `[Pasted ~N lines]` placeholder that expands on submit `[R5]`; file paths pasted as attachments become `@path` mentions.

---

## Phase D — Sessions: dialog, home view, timeline

### D1 [ ] Session list dialog upgrades
- Extend the existing session picker `[R4.2]`: search field (already filters), **pin/unpin** (persisted in a small `sessions.json` state file like `local.tsx` `[R15]`), **quick-switch slots** `<leader>1..9`, and dialog-side **rename**/**delete** with two-step confirm. Categories Pinned → Today → older dates; busy sessions show a spinner glyph.
- **Tests:** filter, pin persistence round-trip, slot assignment/eviction, two-step delete confirm state machine.

### D2 [ ] Home view (config-gated)
- A `home = true` config toggle renders a landing view on boot: ASCII logo (exists in TUI), a prompt box, recent sessions (from the session manager), tips, and footer hints — mirroring `[R8]`. Enter on a session opens it; typing a prompt starts a new session.
- Default off (`home = false`) so existing behavior is preserved; the home route is a mode in the app loop, not a second binary.

### D3 [ ] Timeline + fork-from-timeline
- `/timeline` (`<leader>g`): a picker of user messages (newest first); selecting jumps to that block. `/fork` then forks the session at that message: truncate the chat model after the block (`tui_chat_truncate_after` exists), branch via the session manager's branch path, and prefill the prompt with the message text `[R4.3]`. This reuses `/branch` + `/regen` machinery.
- **Tests:** timeline item assembly (user-message extraction), fork-at-index block math, prompt prefill.

### D4 [ ] Export options dialog
- Extend `/export` with a checkboxes modal: include thinking, include tool details, include error context `[R4.1]`; writes markdown via the existing export path.

---

## Phase E — Safety and question prompts

### E1 [ ] Permission prompt: allow once / always / reject (+ patterns, reject message)
- Redesign `TUI_MODAL_APPROVAL` into opencode's three-option prompt `[R6.4]`:
  - **Allow once** — approve this call.
  - **Always** — persist a rule for the tool+pattern into the safety config (`safety` module already has an approval-list store); a confirm stage lists the exact patterns, matching opencode's "Always allow" stage.
  - **Reject** — deny; an optional second stage "Tell OpenCode what to do differently" collects a message that is returned to the agent as the tool result (existing reject path returns a message today — surface it in the UI).
- Rich per-type bodies: render `edit` as a diff preview, `bash` as a command block, `webfetch` as a URL — from the args JSON already carried in the event (`TUI_EV_APPROVAL`). **This is a safety-boundary change; treat as the highest-review item in the plan.**
- **Tests:** modal state machine (once/always/reject/cancel), pattern-list assembly, reject-message round-trip, cannot-dismiss invariant preserved (no Esc=allow).
- **Verify:** regression test that the old binary-only y/n path is gone; manual approval flows signed off in V-check.

### E2 [ ] Question prompt: tabs, options, multi-select, review
- Extend `TUI_MODAL_ASK_USER` into opencode's question prompt `[R6.5]`: one tab per question + Confirm tab, numbered options, `Type your own answer` custom field, multi-select checkboxes, and a Review tab summarizing answers. Answers returned to the worker as a structured string (existing `answer` field carries it).
- **Tests:** multi-question answer assembly, option numbering, review rendering data, cancel path.

---

## Phase F — Sidebar

### F1 [ ] Sidebar shell
- A right-hand panel (width ~40, toggle `<leader>b`, auto-show at width > 120 like `[R7]`) with a stack of sections and a footer. Each section is a pure-model provider + a thin renderer, so sections are independently testable.

### F2 [ ] Context tokens/cost section
- Show context usage (token count, % of window, est. cost) from the agent's context state `[R7]` — the agent already tracks context; expose a read-only snapshot. **Tests:** formatting/percent math.

### F3 [ ] Modified files section
- List files changed this session from `change_tracker` + session diff with `+adds -dels` `[R7]`; clicking a file opens... (placeholder: prints the file's diff into the chat or future diff viewer G1). **Tests:** change-summary aggregation.

### F4 [ ] Todo section
- Surface the agent's todo items when present (from `tool_memory`/notes state if the run produced one) `[R7]`, `[✓]/[•]/[ ]` glyphs. **Tests:** item-state rendering model.

### F5 [ ] MCP / tools status section
- List MCP/tool enablement + status from the registry with status dots `[R7]`. **Tests:** status-string mapping.

---

## Phase G — Diff viewer

### G1 [ ] Diff view route
- A fullscreen overlay (`<leader>d`, slash `/diff`) rendering the change-tracker/session diff: file tree, unified+split patch panes, next/prev hunk (`]`/`[`), next/prev file (`n`/`p`), mark-reviewed (`m`), toggle file tree (`b`), help (`?`) — mirroring `[R9]`. Data from the session's stored diffs / change_tracker snapshots.
- Pure model: hunk indexing, file tree folding, reviewed-set state — all unit-tested. Rendering via notcurses planes.
- **Tests:** patch parsing (reuse `tui_chat_wrap`), hunk navigation math, file-tree fold state.

### G2 [ ] Revert/undo integration
- From the diff view, `/undo`/`/redo` keep working through change_tracker; the viewer is read-only (matches opencode `[R9]` "no accept/reject of individual hunks").

---

## Phase H — Model, theme, status-bar polish

### H1 [ ] Model picker dialog + cycling keys
- Replace the menu's provider/model picker with a richer dialog: **Recent** models (persisted, capped), **favorites** (toggleable via a `Favorite` action `[R4.2]`), then all provider models; `f2`/`shift+f2` cycle recent/favorite like `[R2.1]`. `ctrl+t` cycles reasoning effort (echo-ai's analogue of opencode's variants `[R2.1]`, stated deviation: no variant concept exists).
- **Tests:** recent/favorite persistence, cycle ordering, picker assembly.

### H2 [ ] Theme list dialog + more presets
- `/theme` opens a picker with live preview (apply-on-move, revert-on-cancel, matching `[R13]`). Add presets to `tui_theme.c` (target ~8–10 from the existing palette engine: current 4 + e.g. `solarized`, `gruvbox`, `tokyonight`, `nord`, `monokai`, `dracula`) — all generated from the same role→palette map, no per-theme code.
- **Tests:** palette generation, preview state machine, config round-trip.

### H3 [ ] Status bar polish
- Add cost/context usage and a queued-prompt indicator to the status bar `[R2.10]`; keep the existing spinner/model/provider/session/tools fields.

---

## Phase I — Stretch / defer

Each deferred item lists the reason. These stay visible in the plan until decided — never silently skipped.

- **I1 [defer] OS notifications + attention sounds** `[R11]` — requires platform notification plumbing and an audio path; no user request yet. Revisit after G.
- **I2 [defer] Tips system** `[R12]` — content authoring cost high, value low until the home view (D2) is live.
- **I3 [defer] Which-key panel** `[R2.8]` — opencode ships it disabled by default; add after the keymap (A) and palette (B) prove the data source.
- **I4 [defer] Plugin system** `[R10]` — a C plugin runtime is a large, separate program; the slot architecture (sidebar F1, palette B2) is designed so a later plugin layer can register into it without rework.
- **I5 [defer] Share/unshare** `[R3]` — needs a server-side share backend; out of TUI scope.
- **I6 [defer] Editor context / SSE editor link** `[R17]` — no editor-connection protocol exists; revisit when an editor integration is requested.
- **I7 [defer] Subagent navigation** `[R6.6]` — subagent *sessions* are not a first-class model in echo-ai today (the `delegate` tool runs agents but doesn't expose session trees); requires agent/session groundwork.
- **I8 [defer] Agent switching (tab/shift+tab)** `[R2.1]` — single configured agent; requires a multi-agent session model.
- **I9 [defer] Prompt frecency** `[R5]` — nice-to-have ranking; add on top of C1 once file indexing exists.
- **I10 [defer] Toasts** `[R4.4]` — the inline `chat_notice` covers it; a transient toast layer can replace it once a notification surface exists (I1).
- **I11 [defer] Conceal / timestamps toggle** `[R2.2]` — low value; timestamps can piggyback on the status-bar work (H3) if desired.

---

## Testing plan (AGENTS.md obligations)

### X1 [ ] Module-split rule
- Only pure-model code is unit-tested. New modules are model-first (`tui_keys`, `tui_command`, `tui_autocomplete`, `tui_diff`, sidebar section models, prompt persistence); render functions are exercised by the V-check only. No Check test inits notcurses.

### X2 [ ] Suites (registered in tests/tui/CMakeLists.txt, one TCase per behavior area, ck_assert_* only)

| Suite | Coverage |
|---|---|
| tests/tui/test_tui_keys.c (A) | key-string parse, aliases, precedence, override/disable, leader chord, timeout |
| tests/tui/test_tui_command.c (B) | registry, aliases, palette data, unknown fallback |
| tests/tui/test_tui_prompt.c (C) | history persistence, stash, shell-mode transitions, autocomplete ranking/insertion |
| tests/tui/test_tui_session_ui.c (D) | pin persistence, quick-switch slots, two-step delete, timeline assembly, fork math, export options |
| tests/tui/test_tui_permission.c (E) | once/always/reject state machine, pattern list, reject message, no-dismiss invariant |
| tests/tui/test_tui_question.c (E) | tabs/options/multi-select/review assembly |
| tests/tui/test_tui_sidebar.c (F) | section providers (context %, file deltas, todo states, status strings) |
| tests/tui/test_tui_diff.c (G) | patch parse, hunk nav, file-tree fold, reviewed set |
| tests/tui/test_tui_theme.c (H, extends existing) | preset generation, preview state machine |

### X3 [ ] Fault injection (AGENTS.md documented pattern)
- Multi-allocation commit sites in this plan: command registry registration (B1), autocomplete option assembly (C1), prompt-history append (C2), sidebar section list build (F1), diff file-tree build (G1). Each gets a `*_TEST` compile guard redefining the allocator hooks and a test that forces failure at each allocation point and asserts: no partial commit, state unchanged, no leaks (sanitizer-verified). Follow the exact `METRICS_TEST` pattern from AGENTS.md and the existing `TUI_EVENTS_TEST` in the tree.

### X4 [ ] Registration and CI
- New suites added to tests/tui/CMakeLists.txt with their own `add_test()` and `tcase_set_timeout` where blocking semantics are exercised. CI runs all three environments under ASan+UBSan per existing convention.

### X5 [ ] Regression discipline
- Every bug found during this work ships with a regression test that fails on the old code and passes on the new; fail→pass evidence archived in docs/verification/ and tracked in this plan's status.

### X6 [ ] Manual V-check (archive in docs/verification/)
- Leader-chord timing, palette filtering UX, permission prompt under real tool calls, question tabs, sidebar at 80/120/200 cols, diff view navigation, theme live preview, external editor round-trip, resize at every phase. Use `script`/`tmux` for the tty (notcurses needs a real terminal; see TUI_MIGRATION_PLAN V1).

---

## Risks and mitigations

1. **Scope is large by design.** Phases A–C are the interaction backbone and must land in order; D–H layer on top. Each phase is independently shippable and testable; the plan is milestone-ordered so a partial pass still leaves a coherent TUI.
2. **Permission-prompt redesign touches the safety boundary (E1).** It only *expands* user choice (once/always-with-patterns/reject-with-message); the underlying deny path and safety gates are unchanged. Highest review priority; signed off in V-check before merging.
3. **`$VISUAL/$EDITOR` spawning (C4)** re-enters raw-mode handling; the renderer suspend/resume is the riskiest render path. Mitigated by reusing the fork pattern from tools/bash.c and by the X6 V-check.
4. **Quick-switch slots and pins (D1)** touch session-manager state; session_manager is mutex-protected and UI-thread-safe (TUI_MIGRATION_PLAN decision 8), so no new locking.
5. **Keymap refactor (A1)** is a behavior-preserving move first — every existing key gets a registry entry — so a regression is a build/test failure, not a UX surprise. Land it as its own commit.
6. **Home view (D2)** is config-gated off by default, so no user-visible change until opted in.

## Definition of done

- **A–C:** keymap, palette, prompt composer (autocomplete/history/stash/shell/editor) live and tested; all X2/X3 suites green under sanitizers; evidence archived.
- **D–E:** session dialog, home view, timeline/fork, export options, permission + question prompts live; safety E1 V-check signed off.
- **F–H:** sidebar, diff viewer, model/theme/status polish live.
- **I:** every defer item has a reason and stays visible.
- Full pass: manual V-check archived; docs/verification/ updated per item.