# OpenCode TUI — Comprehensive Feature Review

**Date:** 2026-08-17
**Repo reviewed:** `https://github.com/anomalyco/opencode` @ `a97fec8` (cloned to `/tmp/opencode-clone`)
**Scope:** `packages/tui` (185 source files) + the TUI-facing surface of `packages/opencode`, `packages/plugin`, `packages/core`, and `packages/protocol`.
**Method:** 4 parallel read-only exploration agents; every file under `packages/tui/src` was read. No files in the reviewed repo were modified.
**Commit verified:** `a97fec8 fix: codex data residency (#42432)`

This document is an exhaustive inventory of the opencode terminal UI: keybindings, menus, dialogs, buttons, prompt features, views, sidebar panels, the diff viewer, plugin system, themes, notifications, config surface, and persistence. Everything below was extracted from source with `file:line` references. Keybinding defaults live in `packages/tui/src/config/keybind.ts` (the `Definitions` object, lines 45–240, mapped to commands via `CommandMap` lines 256–420).

---

## 1. Executive overview

The TUI is a SolidJS + `@opentui/solid`/`@opentui/keymap` application rendered to the terminal by a custom renderer. It connects to an opencode server process over an SDK client (SSE events batched at 16 ms, exponential backoff 1s→30s). Everything the user sees is produced through a **plugin-slot system**: even the home footer, sidebar panels, tips, notifications, diff viewer, and which-key panel are "internal" plugins registered in the same registry external plugins use.

- Input model: `@opentui/keymap` with a leader-key concept. `LEADER_TOKEN = "leader"` (`keymap.tsx:20`), default leader `ctrl+x` (`keybind.ts:41`). `<leader>q` means "ctrl+x then q" with a 2000 ms leader timeout (`config/index.tsx:21`). Key aliases expand `enter→return`, `esc→escape`, `pgdown→pagedown`, `pgup→pageup` (`keymap.tsx:112-126`).
- Mode stack: `"base"` is the normal mode; `"modal"` is pushed while a dialog is open; `"question"` while a question prompt is active; `"autocomplete"` while the autocomplete panel is open (`keymap.tsx:53-100`).
- Two routes plus plugin routes: `home`, `session`, and any route registered by a plugin (`context/route.tsx`). Initial route from `OPENCODE_ROUTE` env or `--continue` flag.

---

## 2. Keybindings — complete default table

### 2.1 App / global (registered in `app.tsx`, active in base mode)

| Key(s) | Command | Action | Def |
|---|---|---|---|
| `ctrl+p` | `command.palette.show` | Open command palette | `keybind.ts:57` |
| `<leader>m` | `model.list` | List/switch models | `:121` |
| `f2` | `model.cycle_recent` | Next recent model | `:122` |
| `shift+f2` | `model.cycle_recent_reverse` | Previous recent model | `:123` |
| (none) | `model.cycle_favorite` / `model.cycle_favorite_reverse` | Cycle favorites | `:124-125` |
| `<leader>a` | `agent.list` | List agents | `:129` |
| (none) | `mcp.list` | List MCP servers | `:126` |
| `tab` | `agent.cycle` | Next agent | `:130` |
| `shift+tab` | `agent.cycle.reverse` | Previous agent | `:131` |
| `ctrl+t` | `variant.cycle` | Cycle model variants | `:132` |
| (none) | `variant.list` | List model variants | `:133` |
| (none) | `provider.connect` | Connect a provider | `:127` |
| (none) | `console.org.switch` | Switch console org | `:128` |
| `<leader>s` | `opencode.status` | View status | `:83` |
| (none) | `opencode.debug` | View debug info | `:84` |
| `<leader>t` | `theme.switch` | List themes | `:78` |
| (none) | `theme.switch_mode` | Switch light/dark | `:79` |
| (none) | `theme.mode.lock` | Lock/unlock theme mode | `:80` |
| (none) | `help.show` | Open help dialog | `:58` |
| (none) | `docs.open` | Open docs (https://opencode.ai/docs) | `:59` |
| (none) | `diff.open` | Open diff viewer | `:60` |
| (none) | `workspace.list` | Manage workspaces | `app.tsx:129` |
| (none) | `app.debug` | Toggle debug panel | `:49` |
| (none) | `app.console` | Toggle console | `:50` |
| (none) | `app.heap_snapshot` | Write heap snapshot | `:51` |
| `ctrl+z` | `terminal.suspend` | Suspend terminal (SIGTSTP) | `:223` |
| (none) | `terminal.title.toggle` | Toggle terminal title | `:224` |
| (none) | `app.toggle.animations` | Toggle animations | `:52` |
| (none) | `app.toggle.file_context` | Toggle file context chip | `:53` |
| (none) | `app.toggle.diffwrap` | Toggle diff wrapping | `:54` |
| (none) | `app.toggle.paste_summary` | Toggle paste summary | `:55` |
| (none) | `app.toggle.session_directory_filter` | Toggle session directory filter | `:56` |

**No-mode-restriction globals** (active even in dialogs; `app.tsx:92-104`, bound at `:971-973`):

| Key(s) | Command |
|---|---|
| `<leader>l` | `session.list` |
| `<leader>n` | `session.new` |
| `<leader>1`…`<leader>9` | `session.quick_switch.1`…`.9` |

**Exit** (`app.tsx:975-983`, enabled only when prompt input is empty / not focused): `ctrl+c`, `ctrl+d`, `<leader>q` → `app.exit`. The console layer adds `ctrl+y` copy-selection (`app.tsx:204`).

### 2.2 Session route (`routes/session/index.tsx`)

Base mode (`:116-144`):

| Key(s) | Command | Def |
|---|---|---|
| (none) | `session.share` | `:95` |
| `ctrl+r` | `session.rename` | `:93` |
| `<leader>g` | `session.timeline` | `:91` |
| (none) | `session.fork` | `:92` |
| `<leader>c` | `session.compact` | `:99` |
| (none) | `session.unshare` | `:96` |
| `<leader>u` | `session.undo` | `:147` |
| `<leader>r` | `session.redo` | `:148` |
| `<leader>b` | `session.sidebar.toggle` | `:81` |
| `<leader>h` | `session.toggle.conceal` | `:149` |
| (none) | `session.toggle.timestamps` | `:100` |
| (none) | `session.toggle.thinking` | `:151` |
| (none) | `session.toggle.actions` (tool details) | `:150` |
| (none) | `session.toggle.scrollbar` | `:82` |
| (none) | `session.toggle.generic_tool_output` | `:101` |
| `ctrl+g,home` | `session.first` | `:141` |
| `ctrl+alt+g,end` | `session.last` | `:142` |
| (none) | `session.messages_last_user` | `:145` |
| (none) | `session.message.next` / `session.message.previous` | `:143-144` |
| `<leader>y` | `messages.copy` | `:146` |
| (none) | `session.copy` | `:87` |
| `<leader>x` | `session.export` | `:86` |
| `<leader>down` | `session.child.first` | `:103` |
| `up` | `session.parent` | `:106` |
| `right` | `session.child.next` | `:104` |
| `left` | `session.child.previous` | `:105` |
| `ctrl+b` | `session.background` (background subagents; only when foreground tasks exist) | `:98` |

**No-mode-restriction session globals** (`:146-153`): `pageup,ctrl+alt+b` page up; `pagedown,ctrl+alt+f` page down; `ctrl+alt+y` line up; `ctrl+alt+e` line down; `ctrl+alt+u` half page up; `ctrl+alt+d` half page down. First/last (`ctrl+g,home` / `ctrl+alt+g,end`) only when the editor is unfocused (`:155`).

### 2.3 Prompt / input (`component/prompt/index.tsx`, `keymap.tsx`)

Base-mode palette commands: `prompt.submit` (no default key), `<leader>e` → `prompt.editor` (open external editor, `:77`), `prompt.editor_context.clear`, `prompt.stash`/`prompt.stash.pop`/`prompt.stash.list`, `prompt.skills`, `workspace.set` (slash `/warp`), `session.move`. `escape` → `session.interrupt` (press twice within 5 s to abort, `:97`; `prompt/index.tsx:393-422`).

**Textarea-focused input layer** (all `input.*` commands, defaults):

| Key(s) | Command |
|---|---|
| `return` | `input.submit` (`:163`) |
| `shift+return,ctrl+return,alt+return,ctrl+j` | `input.newline` (`:164`) |
| `left,ctrl+b` / `right,ctrl+f` / `up` / `down` | `input.move.left/right/up/down` (`:165-168`) |
| `shift+left` / `shift+right` / `shift+up` / `shift+down` | `input.select.left/right/up/down` (`:169-172`) |
| `ctrl+a` / `ctrl+e` | `input.line.home` / `input.line.end` (`:173-174`) |
| `ctrl+shift+a` / `ctrl+shift+e` | `input.select.line.home` / `select.line.end` (`:175-176`) |
| `alt+a` / `alt+e` | `input.visual.line.home` / `visual.line.end` (`:177-178`) |
| `alt+shift+a` / `alt+shift+e` | `input.select.visual.line.home/end` (`:179-180`) |
| `home` / `end` | `input.buffer.home` / `buffer.end` (`:181-182`) |
| `shift+home` / `shift+end` | `input.select.buffer.home/end` (`:183-184`) |
| `ctrl+shift+d` | `input.delete.line` (`:185`) |
| `ctrl+k` | `input.delete.to.line.end` (`:186`) |
| `ctrl+u` | `input.delete.to.line.start` (`:187`) |
| `backspace,shift+backspace` | `input.backspace` (`:188`) |
| `ctrl+d,delete,shift+delete` | `input.delete` (`:189`) |
| `ctrl+-,super+z` (+ `ctrl+z` on win32 where suspend is unavailable) | `input.undo` (`:190`, `config/index.tsx:102-111`) |
| `ctrl+.,super+shift+z` | `input.redo` (`:191`) |
| `alt+f,alt+right,ctrl+right` / `alt+b,alt+left,ctrl+left` | `input.word.forward` / `word.backward` (`:192-193`) |
| `alt+shift+f,alt+shift+right` / `alt+shift+b,alt+shift+left` | `input.select.word.forward/backward` (`:194-195`) |
| `alt+d,alt+delete,ctrl+delete` | `input.delete.word.forward` (`:196`) |
| `ctrl+w,ctrl+backspace,alt+backspace` | `input.delete.word.backward` (`:197`) |
| `super+a` | `input.select.all` (`:198`) |

Prompt-focused: `ctrl+v` → `prompt.paste` (`:162`), `ctrl+c` → `prompt.clear` (`:161`, only when input non-empty). Typing `!` at cursor 0 enters **shell mode**; `escape` exits shell mode; `backspace` at position 0 exits shell mode (`prompt/index.tsx:816-860`). `up`/`down` → `prompt.history.previous/next` only at buffer start/end (`:199-200`).

**Autocomplete** (`component/prompt/autocomplete.tsx`): `up,ctrl+p` prev; `down,ctrl+n` next; `escape` hide; `return` select; `tab` complete (`keybind.ts:214-218`).

### 2.4 Diff viewer (`feature-plugins/system/diff-viewer.tsx`)

| Key(s) | Command |
|---|---|
| `j,down` / `k,up` | `diff.down` / `diff.up` |
| `pagedown,ctrl+f` / `pageup,ctrl+b` | `diff.page.down` / `diff.page.up` |
| `m` | `diff.mark_reviewed` |
| `escape,q` | `diff.close` (`:61`) |
| `enter,space` | `diff.toggle` (`:62`) |
| `right` | `diff.expand` (`:63`) |
| `E` | `diff.expand_all` (`:64`) |
| `left` | `diff.collapse` (`:65`) |
| `tab` | `diff.switch_focus` (`:66`) |
| `]` / `[` | `diff.next_hunk` / `diff.previous_hunk` (`:67-68`) |
| `n` / `p` | `diff.next_file` / `diff.previous_file` (`:69-70`) |
| `b` | `diff.toggle_file_tree` (`:71`) |
| `s` | `diff.single_patch` (`:72`) |
| `d` | `diff.switch_source` (`:73`) |
| `v` | `diff.toggle_view` (`:74`) |
| `?` | `diff.help` (`:75`) |

### 2.5 Dialogs

Shared dialog shell (`ui/dialog.tsx:105-137`): `escape` and `ctrl+c` close the top dialog (while any dialog is open, mode `"modal"` is pushed).

**DialogSelect** (used by every list dialog; `ui/dialog-select.tsx:369-483`): `up,ctrl+p` prev; `down,ctrl+n` next; `pageup`/`pagedown` ±10; `home`/`end`; `return` submit; `tab`/`shift+tab` next/previous footer action (`keybind.ts:202-208`).

**DialogPrompt**: `return` → `dialog.prompt.submit` (`:209`).

Dialog action commands and defaults:

| Key | Command | Used by |
|---|---|---|
| `space` | `dialog.mcp.toggle` (`:210`) | MCP dialog |
| `ctrl+m` | `dialog.move_session.new` (`:211`) | Move session |
| `ctrl+d` | `dialog.move_session.delete` (`:212`) | Move session |
| `ctrl+r` | `dialog.move_session.refresh` (`:213`) | Move session |
| `shift+i` | `dialog.plugins.install` (`:221`) | Plugins |
| `space` | `plugins.toggle` (`:220`) | Plugins |
| `ctrl+a` | `model.dialog.provider` (`:119`) | Model dialog |
| `ctrl+f` | `model.dialog.favorite` (`:120`) | Model dialog |
| `ctrl+f` | `session.pin.toggle` (`:107`) | Session list |
| `ctrl+d` | `session.delete` (`:94`) | Session list / workspaces |
| `ctrl+d` | `stash.delete` (`:118`) | Stash |

Other dialog-specific keys:
- `DialogHelp`: `return`/`escape` close.
- `DialogAlert`: `return` confirms.
- `DialogConfirm`: `return` confirms; `left`/`right` switch confirm↔cancel.
- `DialogExportOptions`: `tab` cycles filename→thinking→toolDetails→assistantMetadata→openWithoutSaving; `space` toggles the active option.
- `DialogDebug`: `return` copies debug info.
- `DialogRetryAction`: `left`/`right`/`tab` select; `return` confirms.
- `DialogSessionDeleteFailed`: `left`/`up` delete; `right`/`down` restore; `return` confirm.
- `DialogWorkspaceUnavailable`: `left` cancel, `right` restore.
- `DialogProvider` AutoMethod: `c` copies the `XXXX-XXXX` code.
- Plugin install prompt: `tab` toggles local/global install scope.

### 2.6 Question prompts (`routes/session/question.tsx`, mode `"question"`)

- Edit mode (custom answer): `escape` cancels edit; `return` submits; `ctrl+c` clears the answer.
- Browse mode: `left/h` / `right/l` / `tab` (shift+tab backwards) switch question tabs; number keys `1`–`9` select answer N; `up/k` / `down/j` move; `return` select/submit; `escape` dismiss/reject.

### 2.7 Permission prompts (`routes/session/permission.tsx`)

- Reject stage: `escape` cancels; `return` confirms.
- Main prompt: `left/h` / `right/l` switch Allow once / Allow always / Reject; `return` selects; `escape` rejects; `ctrl+f` → `permission.prompt.fullscreen` (`:219`).

### 2.8 Which-key panel (`feature-plugins/system/which-key.tsx` — **disabled by default**)

All `ctrl+alt+…`: `ctrl+alt+k` toggle; `ctrl+alt+shift+k` layout toggle (dock↔overlay); `ctrl+alt+shift+p` pending preview; `ctrl+alt+left/[` group previous; `ctrl+alt+right/]` group next; `ctrl+alt+up/p` scroll up; `ctrl+alt+down/n` scroll down; `ctrl+alt+pageup`/`ctrl+alt+pagedown` page up/down; `ctrl+alt+home`/`ctrl+alt+end` home/end (`keybind.ts:229-239`).

### 2.9 Misc

- `tips.toggle` — `<leader>h` (`:225`).
- `plugins.list` / `plugins.install` — no default keys.
- `session.queued_prompts` (`<leader>q`, `:102/309`) — defined but used by the `opencode run` footer (`packages/opencode/src/cli/cmd/run/footer.view.tsx:563`), not the TUI.
- Keymap addons from `@opentui/keymap`: comma bindings, base-layout fallback, timed leader, escape clears pending sequence, backspace pops pending sequence (`keymap.tsx:214-232`).

### 2.10 Scoping summary

- **Global (no mode restriction):** `app.global`, `session.global`, `session.global.unfocused`.
- **Base mode:** all app, session, prompt, diff, permission bindings (off while a dialog/question is open).
- **Prompt-focused:** `input.*`, `prompt.paste`, `prompt.clear`, shell mode, history, autocomplete.
- **Dialog-focused:** dialog shell, `dialog.select.*`, `dialog.prompt.*`, action commands.
- **Route-scoped:** session bindings only on the session route; diff bindings only while the diff route renders.

Commands with **no default key** (reachable via palette/slash): `help.show`, `docs.open`, `diff.open`, `prompt.submit`, `prompt.stash*`, `prompt.skills`, `workspace.set`, `session.move`, `session.copy`, `session.share/unshare`, `session.fork`, `session.toggle.*`, `mcp.list`, `provider.connect`, `console.org.switch`, plugin manager, `app.debug`, `app.console`, `app.heap_snapshot`, `app.toggle_*`, theme mode/lock, scrollbar toggle, status/debug view.

---

## 3. Command palette and slash commands

The command palette (`ctrl+p`, title **"Commands"**, `component/command-palette.tsx`) lists every reachable `namespace: "palette"` command grouped by category, each row showing its keybinding; when the filter is empty a **"Suggested"** category shows commands flagged `suggested: true`. Selecting dispatches the command.

Slash names come from `slashName`/`slashAliases` on palette commands (`useCommandSlashes`, `keymap.tsx:260-290`). Server-side custom commands (`.opencode/commands/*.md`) are dispatched through `session.command` when the submitted first line matches (`prompt/index.tsx:1071-1091`).

**App-level slash commands** (`app.tsx:559-960`):

| Slash | Aliases | Command |
|---|---|---|
| `/sessions` | `resume`, `continue` | session list |
| `/new` | `clear` | new session |
| `/workspaces` | — | workspace list (hidden unless flag) |
| `/models` | `mo` | model list |
| `/agents` | — | agent list |
| `/mcps` | — | MCP list |
| `/variants` | — | variant list |
| `/connect` | — | connect provider |
| `/org` | `orgs`, `switch-org` | switch console org (only when switchable) |
| `/status` | — | status dialog |
| `/debug` | — | debug dialog |
| `/themes` | — | theme list |
| `/help` | — | help dialog |
| `/exit` | `quit`, `q` | exit |

**Prompt-level**: `/editor` (external editor), `/skills` (skill picker, inserts `/{skill} `), `/warp` (workspace), `/move` (move session).

**Session-level** (`sessionCommandList`, `routes/session/index.tsx:466-1085`):

| Slash | Aliases | Command |
|---|---|---|
| `/share` | — | share |
| `/rename` | — | rename |
| `/timeline` | — | timeline |
| `/fork` | — | fork |
| `/compact` | `summarize` | compact |
| `/unshare` | — | unshare |
| `/undo` | — | undo |
| `/redo` | — | redo |
| `/timestamps` | `toggle-timestamps` | toggle timestamps |
| `/thinking` | `toggle-thinking` | toggle thinking |
| `/copy` | — | copy transcript |
| `/export` | — | export transcript |

**Plugin-level**: `/diff` (diff viewer).

**Other prompt triggers** (`prompt/index.tsx:963-967`): literal `exit`, `quit`, `:q` as the full prompt exit; leading `!` runs a shell command (shell mode); `@` prefix triggers file/mention autocomplete; `@agent-name` invokes subagents; backticks inject shell output.

---

## 4. Dialogs and overlays — full inventory

### 4.1 Dialog infrastructure

- **`Dialog`** (`ui/dialog.tsx`) — backdrop + panel wrapper. Sizes `xlarge` 116 cols / `large` 88 / `medium` 60 (`:22-26`). Click-outside closes unless a text selection was in progress; `escape`/`ctrl+c` close (only when nothing is text-selected).
- **`DialogProvider`/`useDialog`** (`ui/dialog.tsx:182-231`) — dialog stack; pushes `"modal"` mode; full-screen capture box implements **copy-on-select** (right-click copy when `OPENCODE_EXPERIMENTAL_DISABLE_COPY_ON_SELECT`; otherwise mouse-up copies the selection + "Copied to clipboard" toast). Only the top of the stack renders.
- **`DialogSelect`** (`ui/dialog-select.tsx`) — the generic list picker used by nearly every dialog: filter input (`placeholder ?? "Search"`), fuzzysort over `title` + `category` (title weighted 2×), footer action chips navigable with `tab`/`shift+tab`, empty state `"No results found"`, current option `●`, `flat` mode collapses grouping when filtering.
- **`DialogPrompt`** — 3-row textarea, busy state with `Spinner` + `"Working..."`/`processing...`.
- **`DialogAlert`** — title + message + `ok` button; `return`/`esc`.
- **`DialogConfirm`** — `cancel`/`confirm` buttons (label overridable); `left`/`right`/`return`.
- **`DialogExportOptions`** — filename textarea (prefilled `session-<id8>.md`) + four `[x]` checkboxes: Include thinking, Include tool details, Include assistant metadata, Open without saving; `tab` cycles focus, `space` toggles.
- **`DialogHelp`** — "Press {ctrl+p} to see all available actions and commands in any context." + `ok`.

### 4.2 Component dialogs

| Dialog | Title | Contents / behavior | Opened by |
|---|---|---|---|
| **DialogAgent** | "Select agent" | one option per agent, `"native"` or agent description | `agent.list` (`<leader>a`, `/agents`) |
| **DialogConsoleOrg** | "Switch org" | orgs grouped by account (`email host`); `"Loading orgs..."`/`"No orgs found"`/`"Could not load orgs"`; switching disposes the instance + toast | `console.org.switch` (`/org`) |
| **DialogDebug** | — | Version, Date, OS, Terminal, Session ID, Model (`providerID/modelID`); **copy** button / `return` copies `"Label: value"` to clipboard + toast; "Share this when reporting an issue." | `opencode.debug` (`/debug`) |
| **DialogMcp** | "MCPs" | one row per MCP with status footer `✓ Enabled`/`○ Disabled`/`⋯ Loading`; action **toggle** (`space`); select deliberately does not close | `mcp.list` (`/mcps`) |
| **DialogModel** | "Select model"/provider name | categories **Favorites** → **Recent** → all provider models → **Popular providers** (top 6 when disconnected); `-nano` models disabled; footer `"Free"` for free opencode models; actions **Connect provider** (`ctrl+a`) and **Favorite** (`ctrl+f`); chains to variant dialog when variants exist | `model.list` (`<leader>m`, `/models`) |
| **DialogMoveSession** | "Move session" | project roots + existing subdirectory locations, categories `"Current"`/`"Other"`; actions **new** (`ctrl+m`), **delete** (`ctrl+d`, two-step confirm, blocked by `DialogWorkspaceFileChanges` when dirty), **refresh** (`ctrl+r`); deleting the checked-out copy navigates home | `session.move` (`/move`) |
| **DialogProvider** | "Connect a provider" | provider rows with `(Recommended)`/`(API key)`/`(ChatGPT Plus/Pro or API key)`/`Low cost subscription...` descriptions, categories `"Popular"`/`"Providers"`, `✓` for connected; auth methods: **oauth** (prompts + `XXXX-XXXX` code with `c` to copy, polls callback), **code** (auth-code prompt, `"Invalid code"` error), **api** (API-key prompt; marketing copy for `opencode` "OpenCode Zen…" and `opencode-go` "$10 per month…"); `"Other"` custom provider validates `^[a-z0-9][a-z0-9-_]*$`; on success chains to `DialogModel` | `provider.connect` (`/connect`); auto-opens when zero providers after sync |
| **DialogRetryAction** | — | non-blocking retry panel with `Link` + animated `BgPulse` (Go upsell art, 30 fps) for `opencode.ai/go`; buttons **don't show again** and action label; auto-opens on `session.status` retry events for `opencode`/`opencode-go` with reasons `free_tier_limit`/`account_rate_limit`, once per 24 h | auto |
| **DialogSessionDeleteFailed** | "Failed to Delete Session" | two cards: **Delete workspace** / **Restore to new workspace**; restore warps the session into a new workspace | from session-list delete failure |
| **DialogSessionList** | "Sessions" | debounced search (150 ms), categories **Pinned** → **Today** → date strings; busy spinner or quick-switch **slot number** in gutter; current session `●`; actions **pin/unpin** (`ctrl+f`), **delete** (`ctrl+d`, two-step), **rename** (`ctrl+r`); footer hint `switch <leader>1-9` | `session.list` (`<leader>l`, `/sessions`) |
| **DialogSessionRename** | "Rename Session" | `DialogPrompt` prefilled with title | `session.rename` (`ctrl+r`) |
| **DialogSkill** | "Skills" | `placeholder="Search skills..."`; selecting inserts `/{skill} ` into the prompt | `prompt.skills` (`/skills`) |
| **DialogStash** | "Stash" | newest-first entries (first line truncated 50, relative time, `~N lines` for multiline); action **delete** (`ctrl+d`); selecting restores + removes | `prompt.stash.list` (palette) |
| **DialogStatus** | "Status" | read-only `•` inventory: **MCP Servers** (status-colored dots; connected/error/"Disabled in configuration"/"Needs authentication (run: opencode mcp auth {key})"/client-registration error), **LSP Servers** (id + root, green/red dot), **Formatters** (enabled list or "No Formatters"), **Plugins** (name + `@version` or "No Plugins") | `opencode.status` (`<leader>s`, `/status`) |
| **DialogTag** | "Autocomplete" | top-5 file search results; **orphan — not referenced anywhere else in the tree** | — |
| **DialogThemeList** | "Themes" | alphabetical; **live preview** on move/filter, revert to original on cancel | `theme.switch` (`<leader>t`, `/themes`) |
| **DialogVariant** | "Select variant" | first option `"Default"` (variant `undefined`); `"No variants available"` toast when empty | `variant.list` (`/variants`) |
| **DialogWorkspaceSelect** | "Warp" | **New workspace** (one per adapter) + **Choose workspace** (`"None"` = local project, recent connected workspaces, `"View all workspaces"` → Existing Workspace dialog); warping shows `DialogAlert` "Unable to Warp Session" on `VcsApplyError`; success injects a synthetic warp-reminder part + `Warped to {name}` notice | `workspace.set` (`/warp`) |
| **DialogWorkspaceFileChanges** | "File Changes Found" | scrollable file list (max height 8) with status letters `A`/`D`/`M` and `+N`/`-N` counts; `no`/`yes` buttons | move/warp/delete confirmations |
| **DialogWorkspaceList** | "Workspaces" | sorted by name, `●` green connected / red, footer = type, details expand on select; action **delete** (`ctrl+d`, two-step) | `workspace.list` (`/workspaces`, hidden unless `OPENCODE_EXPERIMENTAL_WORKSPACES`) |
| **DialogWorkspaceUnavailable** | "Workspace Unavailable" | "This session is attached to a workspace that is no longer available."; `cancel`/`restore` buttons; restore opens the workspace select | auto on prompt submit with disconnected workspace |

### 4.3 Session-route dialogs

- **DialogTimeline** ("Timeline") — one option per user message (newest first, footer = time); `onMove` scrolls the message view; selecting opens **Message Actions**.
- **DialogForkFromTimeline** ("Fork session") — first option **"Full session"**; then fork-at-message options that rebuild a `PromptInfo` and navigate with the prompt pre-filled.
- **DialogMessage** ("Message Actions") — **Revert** ("undo messages and file changes", also restores the prompt), **Copy** ("message text to clipboard"), **Fork** ("create a new session"). Opened by clicking a user message (when nothing selected).
- **DialogSubagent** ("Subagent Actions") — single option **Open**; **orphan — unreferenced in this tree**.

### 4.4 Shared UI primitives

- **Toast** — top-right panel, border colored by variant `info|success|warning|error`, default duration 5000 ms, one at a time; unknown errors map to "An unknown error has occurred".
- **Link** — clickable text that opens the default browser.
- **Border** — `EmptyBorder`, `SplitBorder` (`vertical: "┃"`).
- **Spinner** — braille frames at 80 ms; falls back to `⋯` when animations disabled. Separate `ui/spinner.ts` implements the Knight-Rider scanner animation (diamonds/blocks glyphs, bidirectional hold frames) used for the prompt-status spinner.
- **Logo** — two-color ASCII-art wordmark (`_`→shadow space, `^`→`▀`, `~`→shadow `▀`, `,`→`▄`); shown via `home_logo` slot.
- **StartupLoading** — bottom-centered pill "Loading plugins..." then "Finishing startup..." (held ≥3 s, appears after 500 ms delay, `zIndex 5000`).
- **TodoItem** — `[✓]` completed / `[•]` in_progress (warning tint) / `[ ]` pending.
- **WorkspaceLabel** — `●` icon (colored by status) + `name (type)`.
- **PluginRouteMissing** — "Unknown plugin route: {id}" + `go home` button.
- **ErrorComponent** — full-screen crash screen: "opencode crashed", bordered Error panel, stack-trace scrollbox, actions **Copy report** (`c`), **Restart** (`r`), **Quit** (`q`); copies a pre-built GitHub issue URL (bug-report.yml template, 6000-char cap); hardcoded fallback palette per mode since the theme may have crashed.
- **BgPulse** — custom `FrameBufferRenderable` painting animated "Go" upsell art; throttles renderer to 30 fps.

---

## 5. Prompt composer (`component/prompt/index.tsx`, 1716 lines)

- Multiline textarea, `maxHeight = config.prompt.max_height ?? max(6, height/3)`, syntax highlighting. Placeholders rotate: normal `Ask anything... "Fix a TODO in the codebase"` etc.; shell `Run a command... "ls -la"` etc.
- Prompt state model `PromptInfo = { input, mode?: "normal"|"shell", parts }` (`prompt/history.tsx:9-25`). Parts are `file` (with virtual-text source ranges + URL), `agent` (source start/end/value), or `text` (pasted content). Extmarks map parts to virtual text spans (`extmark.file` warning+bold, `extmark.agent` secondary+bold, `extmark.paste` warning bg).
- **Shell mode**: `!` at cursor 0; commands run via `session.shell`.
- **External editor**: `<leader>e` / `/editor` opens `$VISUAL || $EDITOR` on a temp `.md` file with the current prompt, re-syncs part positions, keeps only non-text parts.
- **Editor context chip**: shows `file#12-20` when the editor has a file/selection open and file context is enabled; inserted as a synthetic text part containing `<system-reminder>` describing the opened file/selection; dismissed via `prompt.editor_context.clear`.
- **History**: `prompt-history.jsonl`, max 50, consecutive-dedup, up/down recall (with mode + parts), draft retention (≥20 chars or any parts saved to history on clear).
- **Stash**: `prompt-stash.jsonl`, max 50; `prompt.stash` (save draft), `prompt.stash.pop` (restore last), `prompt.stash.list` (dialog).
- **Frecency**: `frecency.jsonl`, max 1000; score = `frequency / (1 + days since lastOpen)`; boosts autocomplete file ranks.
- **Autocomplete**: `@` (files via `v2.fs.find`, limit 20; MCP resources; non-primary non-hidden agents as `@name`; reference aliases; `file#12` / `file#12-20` line ranges; directory completion via `tab` on a directory inserts `@dir/`) and `/` (slash commands + server commands, `:mcp` suffix for MCP commands, skills excluded). Fuzzysort threshold 0.5 for `@`, 0 for `/`; prefix-exact scores doubled; frecency multiplier; top 10 rows; panel tracks the anchor position every 50 ms.
- **Paste/attachments**: normalizes CRLF→LF; local file paths → `file` parts; images/PDFs become base64 data-URL parts with `[Image {n}]`/`[PDF {n}]` virtual text (numbered per type); SVG read as text → `[SVG: {name}]`; large pastes (≥3 lines or >150 chars) summarized as `[Pasted ~N lines]` (full text kept in the part, expanded on submit); placeholder expansion when copying out of the composer.
- **Submission pipeline**: guards double-submit; `exit`/`quit`/`:q` exits; no model → toast + auto-open provider dialog when zero providers; unavailable workspace → `DialogWorkspaceUnavailable`; new session consults home destination + workspace selection; shell mode → `session.shell`; leading `/` matching a server command → `session.command`; otherwise `session.prompt` with parts. Success appends to history, clears, navigates to the new session after 50 ms.
- **Status line** (bottom): running status (spinner + retry text + `esc interrupt`), workspace notice (`Warped to X`), workspace label (`Creating {type}...`/`Workspace (new {type})`/`Workspace {name}`), move progress, `(new working copy)`, cwd hint; right side: editor-context file chip, usage/cost (`context · cost`), hint buttons `{shortcut} agents`, `{shortcut} commands`, or `esc exit shell mode`.
- **Retry status**: message truncated at 80 chars, `[retrying in {duration} attempt #{n}]`, `(click to expand)` opens "Retry Error" `DialogAlert`.
- `tui.prompt.append` event inserts text from another process.
- Focus management: prompt blurs while a dialog is open and refocuses after.

---

## 6. Session view (`routes/session/index.tsx`)

### 6.1 Layout and message rendering

- Scrollbox with sticky-bottom scrolling and optional vertical scrollbar; permission/question prompts, subagent footer, and prompt live in a flex-fixed bottom area.
- Sidebar visible when `width > 120`; toggled with `<leader>b`; overlays the screen with a dimming backdrop on narrow terminals.
- Persistent UI prefs via KV: `sidebar`, `timestamps`, `tool_details_visibility`, `scrollbar_visible`, `diff_wrap_mode`, `generic_tool_output_visibility`.

### 6.2 Message types

- **Revert banner**: `"{n} message reverted"` + `"<redoShortcut> or /redo to restore"` + per-file `+adds -dels`; click confirms "Confirm Redo".
- **User messages**: left border colored by agent; text from non-synthetic text parts; `" Directory "`/`" File "` chips + filenames; `" QUEUED "` badge (behind a pending assistant); timestamp when enabled; **compaction divider** box titled `" Compaction "`; click (no selection) opens Message Actions.
- **Assistant messages**: parts rendered via `PART_MAPPING` (`text`, `tool`, `reasoning`); footer `▣ <mode> · <model>` + `· <duration>` + `· interrupted`; error banner when the message errored; `{childShortcut} view subagents` and `{backgroundShortcut} background` hints when task parts exist.
- **Reasoning parts**: `[REDACTED]` redaction; header `"Thinking: <title>"` with spinner while running, `"+ / - Thought: <title> · <duration>"` after; collapsed to one line in hide mode, click to expand; hidden entirely when thinking is off.
- **Text parts**: markdown with syntax highlighting, grid tables, `conceal` support.

### 6.3 Tool part rendering

Each tool renders an `InlineTool` one-liner or a `BlockTool` with output. Icons/pending text:

| Tool | Inline icon / pending | Block title |
|---|---|---|
| bash | `$` / `"Writing command..."` | `# Running in <wd>`, `$ <cmd>`, collapsible 10 lines |
| glob | `✱` / `"Finding files..."` | `Glob "<pattern>" in <path> (n matches)` |
| read | `→` / `"Reading file..."` | `↳ Loaded <path>` |
| grep | `✱` / `"Searching content..."` | `Grep "<pattern>" ... (n matches)` |
| webfetch | `%` / `"Fetching from the web..."` | `WebFetch <url>` |
| websearch | `◈` / `"Searching web..."` | `<provider> "<query>" (n results)` |
| write | `←` / `"Preparing write..."` | `# Wrote <file>` + code block + diagnostics |
| edit | `←` / `"Preparing edit..."` | `← Edit <file>` + diff (split when width>120 or `diff_style`, unified otherwise) + diagnostics |
| task | `✓`/`│` / `"Delegating..."` | `{Agent} Task — {desc}`, `↳ Retrying (attempt n)`, `↳ {tool} {title}`, `↳ n toolcalls · duration`; click navigates into child session |
| execute | `✗/✓/│` | `execute` + `↳ <tool> <args> (failed)` |
| apply_patch | `%` / `"Preparing patch..."` | per-file `# Deleted`/`# Created`/`# Moved a → b`/`← Patched <path>` blocks with diffs |
| todowrite | `⚙` / `"Updating todos..."` | `# Todos` block with `TodoItem`s |
| question | `→` / `"Asking questions..."` | `# Questions` block of Q/A |
| skill | `→` / `"Loading skill..."` | inline `Skill "<name>"` |
| generic | `⚙` / `"Writing command..."` | `# <tool> <args>`, collapsed to 3 lines, click to expand/collapse |

Common behavior: inline tools go warning-colored when awaiting permission, red + strikethrough when denied (`denied()` matches `QuestionRejectedError`/`rejected permission`/`specified a rule`/`user dismissed`), red when failed; click to expand errors; tool details can be hidden entirely via `session.toggle.actions`.

### 6.4 Permission prompt (`routes/session/permission.tsx`)

Three-stage fullscreen-or-inline prompt:

1. **`△ Permission required`** — per-permission body built by `info()`: `edit` shows an embedded diff (`"No diff provided"` fallback), `read` shows `Path:`, `glob`/`grep` show `Pattern:`, `bash` shows `# Shell command` + `$ <command>`, `task` shows `# <Type> Task` + `◉ <desc>`, `webfetch` shows `% WebFetch <url>`, `websearch` shows `◈ <provider> "<query>"`, `external_directory` shows `← Access external directory` + Patterns list, `doom_loop` shows "⟳ Continue after repeated failures", fallback `⚙ Call tool <permission>`. Buttons: **Allow once / Allow always / Reject**.
2. **"Always allow"** — for `*` pattern: "This will allow <permission> until OpenCode is restarted."; else lists patterns. Buttons: **Confirm / Cancel**.
3. **Reject stage** — `△ Reject permission`, "Tell OpenCode what to do differently", focused textarea; `enter confirm` / `esc cancel`.

Auto mode (`--auto`/`--yolo`/`--dangerously-skip-permissions`) auto-replies `"once"` to every permission request (`context/sync.tsx:196-206`).

### 6.5 Question prompt (`routes/session/question.tsx`)

Multi-question requests in `"question"` mode: one tab per question + a **Confirm** tab; options numbered `1.`…`n` plus a custom `Type your own answer` option (when `custom !== false`); multi-select appends `(select all that apply)` and renders `[✓]/[ ]` checkboxes; single-select marks with a trailing ` ✓`; the **Review** tab summarizes each answer or `(not answered)` in error color. Footer hints `⇆ tab`, `↑↓ select`, `enter confirm/submit/toggle`, `esc dismiss`.

### 6.6 Subagent footer

Shown when the session has a `parentID`: label parsed from title `/@(\w+) subagent/`, `({index} of {total})` among siblings, token/cost usage `"{tokens} ({pct})"` + `· $cost`; clickable **Parent / Prev / Next** buttons with shortcut hints.

### 6.7 Export / copy

- **Copy**: `session.copy` copies the transcript (markdown); `<leader>y` copies the last assistant message.
- **Export**: `<leader>x` opens `DialogExportOptions` and writes `session-<id8>.md`; transcript format: `# title`, `**Session ID:**`, `**Created/Updated:**`, `## User`, `## Assistant (Agent · Model · 1.2s)`, `_Thinking:_` (when included), `**Tool: name**` with `**Input:**`/`**Output:**`/`**Error:**` blocks (when included).

---

## 7. Sidebar (`routes/session/sidebar.tsx` + feature plugins)

42 columns wide, `backgroundPanel`, scrollbox + fixed footer. Three plugin slots: `sidebar_title` (single winner), `sidebar_content` (multi), `sidebar_footer` (single winner). Default title: session title (bold), session ID (non-latest channel), `WorkspaceLabel`, share URL.

Ordered panels in `sidebar_content`:

1. **Context** (order 100) — `"{n} tokens"`, `"{n}% used"`, `"${cost} spent"` from last assistant message + model context limit.
2. **MCP** (order 200) — `MCP` header with collapsed summary `({n} active, {m} errors)`; rows with status dots: `Connected` / `Failed <error>` / `Disabled` / `Needs auth` / `Needs client ID`; collapsible when >2 servers (`▼/▶`).
3. **LSP** (order 300) — `LSP` header; `"LSPs are disabled"` (config off) or `"LSPs will activate as files are read"`; rows `• <id> <root>` green/red dot; collapsible.
4. **Todo** (order 400) — shown only when a non-completed item exists; renders `TodoItem`s; collapsible.
5. **Modified Files** (order 500) — per file `+adds` (diffAdded) `-dels` (diffRemoved); collapsible.

Sidebar footer (`sidebar/footer.tsx`, order 100): a **Getting started** box when no paid provider and not dismissed (`kv "dismissed_getting_started"`): `⬖`, "OpenCode includes free models so you can start immediately.", "Connect from 75+ providers…", `Connect provider`/`/connect` link, `✕` dismiss; below it the session directory path and `• OpenCode <version>`.

---

## 8. Home view (`routes/home.tsx`)

Centered logo (`home_logo` slot), prompt box capped at `promptMaxWidth` (75 or 70% with `"auto"`), `home_bottom` slot, toast, `home_footer` slot. Placeholders rotate (normal: "Fix a TODO in the codebase", "What is the tech stack of this project?", "Fix broken tests"; shell: "ls -la", "git status", "pwd"). `--prompt` args auto-submit after sync.

- **Session destination** (`routes/home/session-destination.tsx`): `{type:"directory", directory, subdirectory} | {type:"new"}`; default = `sync.path.directory || cwd`; set by the move-session dialog; consumed by the home footer's directory display and by prompt submit for new sessions.
- **Home footer** (`home_footer` slot): `Directory` (abbreviated home path, `:<branch>` when the destination is the cwd), `MCP` (`⊙` colored + `"{count} MCP"` + `/status` hint, only when MCPs exist), `Version` (`api.app.version`).
- **Tips** appear in `home_bottom` (see §11).
- Which-key registers a `home_bottom` hint: "Show keyboard shortcuts with ctrl+alt+k" (when enabled).

---

## 9. Diff viewer (`feature-plugins/system/diff-viewer*.tsx`)

A plugin-registered route (`ROUTE = "diff"`, `api.route.register`), fullscreen overlay `zIndex 2500`. Opened via `diff.open` (slash `/diff`).

- **Sources** (`DiffMode = "git" | "branch" | "last-turn"`): **Working tree** ("Show current git changes"), **Main branch** ("Show changes compared to main branch", only on a non-default branch), **Last turn** ("Show changes from the last assistant turn"); switched with `d` (DialogSelect "Switch source"). Data: VCS diff (12 context lines) or per-message session diff.
- **Layout**: header `Diff <source> + {n} file(s)`; states `Loading diff...` / `No diff!` / `Failed to load diff`; `PanelGroup` with optional file tree (width 32) + patch pane.
- **File tree**: directories `▾/▸`, indentation guides, branch glyphs `├─/└─`, collapsed single-child directory chains, per-file status marker `M/A/D` and `✓` when reviewed; `No files` empty state; keyboard expand/collapse/toggle/expand_all; clickable rows.
- **Views**: split (pane width ≥ 100) vs unified; default respects `diff_style`; `v` overrides; persisted in `kv diff_viewer_view`.
- **Single-patch mode** (`s`), file-tree toggle (`b`), both persisted (`kv diff_viewer_single_patch`, `diff_viewer_show_file_tree`).
- **Hunk jumping** (`]`/`[` finds `@@` lines, tracks `SelectedHunk {fileIndex, hunkIndex, scrollTop}`), **file jumping** (`n`/`p`).
- **Mark reviewed** (`m`): per-file `✓`; reviewed files render desaturated.
- **Help** (`?`): "Diff shortcuts" table (Key/Action/Description rows).
- **Read-only**: no accept/reject of hunks; revert/undo is done via `session.undo`/`session.revert`.
- Footer hint bar: `focus file tree`, `next file`, `next hunk`, `previous hunk`, `previous file`, `switch source`, `mark reviewed`, `all` (help).

---

## 10. Plugin system

### 10.1 Runtime and API

`createPluginRuntime()` (`plugin/runtime.tsx`) provides `commands`, `status`, `slots`, `routes`. `TuiPluginApi` (`packages/plugin/src/tui.ts:581-626`): `app`, `attention`, `command?`, `keys`, `keymap`, `mode`, `route`, `ui`, `tuiConfig`, `kv`, `state`, `theme`, `client`, `event`, `renderer`, `slots`, `plugins`, `lifecycle`. A plugin module is `{ id?, tui, server?: never }`; the TUI host loads internal plugins first, then external (`file://` / npm) plugins, applies `plugin_enabled`, then activates sequentially.

Legacy v1 `api.command` is bridged by a **command shim** (`plugin/command-shim.ts`, "remove in v2") that converts `TuiCommand[]` into palette commands + keymap layers and emits deprecation warnings suggesting `api.keymap.registerLayer(...)` / `api.keymap.dispatchCommand(name)`.

### 10.2 Slot map

`TuiHostSlotMap` (`packages/plugin/src/tui.ts:455-486`):

- `app` — full-screen overlay space
- `app_bottom` — bottom dock
- `home_logo`, `home_prompt` (`{ ref? }`), `home_prompt_right`, `home_bottom`, `home_footer`
- `session_prompt` (`{ session_id, visible?, disabled?, on_submit?, ref? }`), `session_prompt_right` (`{ session_id }`)
- `sidebar_title` (`{ session_id, title, share_url? }`), `sidebar_content` (`{ session_id }`), `sidebar_footer` (`{ session_id }`)

`Slot` context = `{ theme: TuiTheme }`. Plugin errors are logged as `"[tui.slot] plugin error"` with `{plugin, slot, phase, source, message}`.

### 10.3 Built-in plugins

| id | Slot(s) | What it renders |
|---|---|---|
| `internal:home-footer` | `home_footer` | directory + branch, MCP count/error, version |
| `internal:home-tips` | `home_bottom` | rotating tips; `tips.toggle` command |
| `internal:sidebar-context` | `sidebar_content` | token count / % / cost |
| `internal:sidebar-mcp` | `sidebar_content` | MCP server status |
| `internal:sidebar-lsp` | `sidebar_content` | LSP status |
| `internal:sidebar-todo` | `sidebar_content` | todo items |
| `internal:sidebar-files` | `sidebar_content` | modified files |
| `internal:sidebar-footer` | `sidebar_footer` | Getting-started card, path, version |
| `internal:notifications` | (no slots) | events → attention notifications |
| `internal:plugin-manager` | (no slots) | `plugins.list` / `plugins.install` commands + install dialog |
| `which-key` (**disabled by default**) | `home_bottom`/`app`/`app_bottom` | keybinding explorer panel |
| `diff-viewer` | route `"diff"` | git diff viewer |

### 10.4 Plugin manager (`feature-plugins/system/plugins.tsx`)

Commands `plugins.list` ("Plugins") and `plugins.install` ("Install plugin") in the palette namespace. The View is a `DialogSelect` titled "Plugins" listing all plugins with category `Internal`/`External`, description `Built-in plugin` (wide) or spec path, footer `disabled`/`active`/`inactive`; actions **toggle** (`space`) and **install** (`shift+i`) which opens an "Install plugin" `DialogPrompt` (placeholder `npm package name`, `tab` toggles `local`/`global` scope, busy state "Installing plugin..."). Toasts: `Loaded {mod} in current session.`, `Installed plugin, but runtime load failed...`, `Package has no TUI target to load in this app.`.

### 10.5 Plugin state surface

`api.state` exposes: `ready`, `config`, `provider`, `path` (`{state, config, worktree, directory}`), `vcs` (`{branch, default_branch}`), `session.count/get/diff/todo/messages/status/permission/question`, `part(messageID)`, `lsp()`, `mcp()` (sorted, with `error` attached on failure).

---

## 11. Notifications, attention, and audio

`feature-plugins/system/notifications.ts` listens to events and calls `api.attention.notify` (OS notification only when the terminal is blurred; sound always):

| Event | Notification title | Sound |
|---|---|---|
| `question.asked` | "Question needs input" | `question` |
| `permission.asked` | "Permission needs input" | `permission` |
| `session.status` idle (was busy/retry) | "Session done" | `done` / `subagent_done` |
| `session.error` | "Session aborted" (`MessageAbortedError`), "Model stopped responding" (SSE read timeout), or "Session error" | `error` |

`attention.ts`: notifications gated on `config.attention.enabled`, renderer destruction, empty message (skip reasons `attention_disabled | empty_message | blurred | focused | focus_unknown | renderer_destroyed`); limits 80/240 chars, ANSI stripped. Sound packs: builtin `"opencode.default"` ("OpenCode Default") with `bip-bop-01.mp3` (default/done), `bip-bop-03.mp3` (question), `staplebops-06.mp3` (permission), `nope-03.mp3` (error), `yup-01.mp3` (subagent_done). `api.attention.soundboard`: `registerPack`, `activate(id, {persist})` (persists `kv "attention_sound_pack"`), `current()`, `list()`. Audio plays lazily via `Audio.create({autoStart:false})`.

**Clipboard** (`clipboard.ts`): read supports macOS image via `osascript`, Windows/WSL PowerShell, Linux `wl-paste`/`xclip`, text via `clipboardy`; write always emits **OSC52** (`\x1b]52;c;<b64>\x07` with tmux/`STY` passthrough) then platform fallbacks.

---

## 12. Tips (`feature-plugins/home/tips.tsx` + `tips-view.tsx`)

Rotating tips card in the `home_bottom` slot, shown unless `kv "tips_hidden"`; hidden automatically on the very first session when a paid provider is connected. Toggle `<leader>h` (title "Show tips"/"Hide tips"). Fixed tip when not connected: "Run /connect to add an AI provider and start coding". The corpus is ~110 tips covering: `@file` fuzzy attach; `!cmd` shell; Build/Plan agent cycling; `/undo`, `/redo`, `/share`, `/editor`, `/init`, `/models`, `/themes`, `/new`, `/sessions`, `/compact`, `/export`, `/connect`, `/timeline`, `/status`, `/help`, `/rename`, `/review`; session pinning and `<leader>1`–`<leader>9` quick switch; leader key; code concealment; scrolling; paste; interrupt; parent/child nav; `opencode.json`/`tui.json` config, `$schema`, keybinds, `none` to disable, MCP config, `.opencode/commands/`, `$ARGUMENTS`/`$1`, backticks, `.opencode/agents/`, permissions (`"git *": "allow"`, `"rm -rf *": "deny"`, `"git push": "ask"`), formatter, LSP, `.opencode/tools/`, `.opencode/plugins/`, themes, temperature/steps/tools, `share` auto/disabled, `/unshare`, `doom_loop`, `external_directory`, `docker run -it --rm ghcr.io/anomalyco/opencode`, `AGENTS.md`, `opencode run`, `--continue`, `-f file.ts`, `--format json`, `serve`, `--attach`, `upgrade`, `auth list`, `agent create`, `/opencode` GitHub actions, `scroll_acceleration`. Platform-specific: non-Windows gets a terminal-suspend tip; Windows gets an input-undo tip.

---

## 13. Themes

**33 built-in themes** (`theme/index.ts:130-164`): `aura`, `ayu`, `catppuccin`, `catppuccin-frappe`, `catppuccin-macchiato`, `cobalt2`, `cursor`, `dracula`, `everforest`, `flexoki`, `github`, `gruvbox`, `kanagawa`, `material`, `matrix`, `mercury`, `monokai`, `nightowl`, `nord`, `one-dark`, `osaka-jade`, `opencode` (default), `orng`, `lucent-orng`, `palenight`, `rosepine`, `solarized`, `synthwave84`, `tokyonight`, `vesper`, `vercel`, `zenburn`, `carbonfox`. Plus a generated **`system`** theme from the terminal's ANSI palette, and plugin/custom themes from `themes/*.json` under the config dir and every ancestor `.opencode/` dir. Priority: `defaults < plugin installs < custom files < generated system`.

Theme JSON format: optional `$schema`, `defs` (named colors), `theme` with 50 color keys (`selectedListItemText`, `backgroundMenu`, `thinkingOpacity` optional). Colors can be hex, `defs` references, `{dark, light}` variants, or numeric ANSI codes; circular refs throw `"Circular color reference"`, missing refs throw `"Color reference ... not found in defs or theme"`. Syntax styles cover ~90 rule scopes including special TUI scopes `prompt`, `extmark.file`, `extmark.agent`, `extmark.paste`. `SIGUSR2` triggers a theme refresh. Selection stored in `kv "theme"`, mode in `kv "theme_mode"`/`theme_mode_lock`; `config.theme` overrides. Default `opencode` theme: dark accent `#9d7cd8` (purple), light accent `#d68c27` (orange); primary `#fab283`/`#3b7dd8`, secondary `#5c9cf5`/`#7b5bb6`.

---

## 14. Config surface

**File**: `tui.json` (JSONC). Load order: global config dir → `OPENCODE_TUI_CONFIG` override → project `tui.json` files → `.opencode` dirs. Legacy `theme`/`keybinds`/`tui` keys in `opencode.json` are migrated into `tui.json`. Schema URL `https://opencode.ai/tui.json`.

`Info` schema: `$schema`, `theme` (name; `"system"` matches the terminal), `keybinds` (partial overrides), `plugin` (`string | [string, options][]`), `plugin_enabled` (`Record<string, boolean>`), `leader_timeout` (ms, default 2000), `attention` (`{enabled, notifications, sound, volume 0–1, sound_pack, sounds}`), `prompt` (`{max_height, max_width: int | "auto"}`), `scroll_speed`, `scroll_acceleration` (`{enabled}`), `diff_style` (`"auto" | "stacked"`), `cursor` (`{style, blinking}`), `mouse` (default true; `OPENCODE_DISABLE_MOUSE` also applies).

**Keybind override format**: each key can be a string, comma-separated alternatives, leader tokens, a `KeyStroke` object, a binding object, an array, or `false`/`"none"` to disable. Unknown keys throw `Unrecognized keybind`; hosts silently drop unknown keys before loading. On win32, `terminal_suspend` is forced to `"none"` and `ctrl+z` becomes `input_undo`.

**Editor**: no TUI config — `$VISUAL`/`$EDITOR`.

---

## 15. Persistence (state files)

| File | Contents | Limits |
|---|---|---|
| `<state>/prompt-history.jsonl` | prompt history | 50, consecutive-dedup |
| `<state>/prompt-stash.jsonl` | stashed prompts | 50 |
| `<state>/frecency.jsonl` | file frecency | 1000 |
| `<state>/kv.json` | UI prefs (sidebar, timestamps, theme, tips, which-key layout, diff view prefs, attention sound pack, etc.) | — |
| `<state>/model.json` | per-agent model, recent, favorites, variant | — |
| `<state>/session.json` | pinned sessions, quick-switch slots | — |
| config + `.opencode/**/themes/*.json` | custom themes | — |

All writes atomic via temp-file + rename.

---

## 16. Context providers (one line each)

`Exit → Epilogue → ErrorBoundary → TuiPaths → TuiTerminalEnvironment → TuiStartup → Clipboard → OpencodeKeymap → Args → KV → Toast → Route → TuiConfig → PluginRuntime → SDK → Permission → Project → Sync → Data → Theme → Local → PromptStash → Dialog → Frecency → PromptHistory → PromptRef → EditorContext → Location → App`

- **Args** — CLI flags `{model, agent, prompt, continue, sessionID, fork, auto}`.
- **Clipboard** — injectable clipboard service (default `clipboard.ts`).
- **Data** — location-keyed v2 catalog data + per-session streaming message store from `session.next.*` events.
- **Directory** — `<abbreviated-dir>[:branch]` display string.
- **Editor** — WebSocket MCP connection to the editor (port from `CLAUDE_CODE_SSE_PORT`/`OPENCODE_EDITOR_SSE_PORT`, else `.claude/ide/*.lock` discovery, else Zed polling); `initialize` with `MCP_PROTOCOL_VERSION = "2025-11-25"`, methods `selection_changed` and `at_mentioned`.
- **Epilogue** — on-exit stdout text.
- **Event** — SDK event subscription with `{directory, workspace}` metadata.
- **Exit** — destroys the renderer, records reason for stderr printing.
- **KV** — persistent JSON store, flock-guarded.
- **Local** — local UI state: model/agent/session/mcp prefs.
- **Location** — current `LocationRef {directory, workspaceID}`.
- **Path-format** — `path()` + relative/`~`-abbreviated formatting.
- **Permission** — `mode: "auto" | "normal"`.
- **Project** — project id/worktree/mainDir, instance path, workspace list/status.
- **Route** — `{type:"home"|"session"|"plugin", ...}`.
- **Runtime** — TUI paths, terminal environment (`platform`, `multiplexer: "tmux"|"screen"`, `displayServer: "wayland"|"x11"`), startup (`OPENCODE_FAST_BOOT`).
- **SDK** — created client + SSE source (16 ms batching, 1s→30s backoff).
- **Sync** — main server-data store (`status: loading|partial|complete`, providers, agents, commands, permissions, questions, config, sessions, diffs, todos, messages, parts, lsp, mcp, mcp resources, formatters, vcs); session list scoped to subdirectory when `kv "session_directory_filter_enabled"`.
- **Theme** — resolved theme, syntax, mode, lock; auto-detects terminal palette, responds to `CliRenderEvents.THEME_MODE`, ANSI `\x1b[?997;1n/2n`, and SIGUSR2.
- **Thinking** — `ThinkingMode = "show" | "hide"` in `kv "thinking_mode"` (default `"hide"`); `reasoningSummary()` splits OpenAI-style `**Title**` headers.

---

## 17. Editor integrations

- **External editor**: `openEditor()` writes `<tmp>/<timestamp>.md`, suspends the renderer, spawns `$VISUAL || $EDITOR`, reads the result, cleans up, resumes + re-renders.
- **Claude-style connection**: `discoverEditorConnection(directory)` scans `~/.claude/ide/*.lock` for a WebSocket transport, picks the highest-scoring workspace folder match.
- **Zed**: `resolveZedSelection(dbPath, cwd)` queries the Zed SQLite DB (`items`, `panes`, `workspaces`, `editors`, `editor_selections`) for the active editor selection; returns `{type:"selection"|"empty"|"unavailable"}` with `source:"zed"` ranges. `OPENCODE_ZED_DB` overrides the DB path. `isZedTerminal()` checks `ZED_TERM`/`TERM_PROGRAM`.

---

## 18. CLI flags affecting the TUI

From `packages/opencode/src/cli/cmd/tui.ts:287-295`:

| Flag | Effect |
|---|---|
| `--continue` / `-c` | loads sessions, navigates to the most recently updated root session |
| `--session` / `-s` (+ `--fork`) | forks after sync completes |
| `--agent` | sets local agent selection |
| `--model` | sets local model selection |
| `--prompt` / positional | pre-fills the home prompt; auto-submits |
| `--auto` / `--yolo` / `--dangerously-skip-permissions` | flips permission mode to auto (auto-allow) |

Also relevant: `OPENCODE_ROUTE` (initial route), `OPENCODE_FAST_BOOT` (skip initial loading), `OPENCODE_DISABLE_MOUSE`, `OPENCODE_EXPERIMENTAL_DISABLE_COPY_ON_SELECT`, `OPENCODE_EXPERIMENTAL_WORKSPACES`, `OPENCODE_TUI_CONFIG`, `OPENCODE_EDITOR_SSE_PORT`, `OPENCODE_ZED_DB`.

---

## 19. Exit behavior

On exit (`app.tsx:186-363`): the renderer is destroyed, the shutdown deferred resolves, `exit.reason` (if set) is printed to stderr via `cliErrorMessage ?? errorFormat`, and the epilogue is printed to stdout. The session route sets the epilogue to `sessionEpilogue({title, sessionID})` — the ASCII "opencode" wordmark + `Session <title>` / `Continue opencode -s <id>` — on mount and clears it on cleanup.

---

## 20. Notable absences / oddities discovered

- `DialogTag` ("Autocomplete") and `DialogSubagent` ("Subagent Actions") are defined but **unreferenced anywhere else in the tree** — orphan components.
- The which-key plugin ships **disabled** by default; must be enabled via `plugin_enabled` in `tui.json`.
- `session.queued_prompts` (`<leader>q`) has a definition and keybind but no registered TUI command — it is consumed by the `opencode run` command footer, not the TUI.
- The diff viewer is **read-only** — no per-hunk accept/reject; revert happens via `session.undo`/`session.revert`.
- The prompt-composer `DialogTag`-style file autocomplete is not wired; file tagging goes through the `@` autocomplete instead.