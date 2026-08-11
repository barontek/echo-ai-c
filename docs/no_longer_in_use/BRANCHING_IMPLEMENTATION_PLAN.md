# Branch-Based Editing & Regeneration — Implementation Plan

Status: implemented 2026-08-07; amended 2026-08-08 (see §9 Amendments)
Date: 2026-08-07
Supersedes: FRONTEND_BACKEND_DISCREPANCIES.md item 9 (`edit` dead `message_id` — the id plumbing is completed as part of this feature)

## 1. Goal

Editing or regenerating a message must **fork** the conversation instead of
destroying the tail, like ChatGPT:

- The old tail is preserved forever as a switchable branch.
- A `‹ 1/k ›` pill renders **below the message where the fork happened**
  (inline under that message; multiple pills along one chain when it forked
  multiple times).
- Clicking `›`/`‹` cycles to the next/previous branch; arrows are **disabled
  at bounds** (no wrap-around). Pill is **hidden when k ≤ 1**.
- No per-branch labels — just the counter.
- **Regenerate** exists in two places, both forking:
  1. A button on the **last assistant message** (ChatGPT-style, e.g. in the
     composer's action row after a completed response).
  2. The edit box's existing **"Regenerate"** button — forks that turn with
     the original text (no content change).
- Editing or regenerating inside an **older branch** (after switching) is
  allowed and forks from that branch.

## 2. Data model ("tree of chains")

### Message fields (persisted per message)

- `id` — already exists (`Message.id`, src/agent/message.h:7). Stable identity.
- `parent_id` — NEW. `id` of the preceding message in this message's chain
  (NULL for the chain root).
- `fork_group_id` — NEW. Logical node identity. All messages that represent
  the same fork point across chains share one `fork_group_id`. Minted when a
  fork happens (edit/regenerate); the pre-fork message keeps its old
  `fork_group_id`.

### Session layout

- `session->messages` — the **active chain** (root→leaf), today's flat array
  plus the two new fields. Streaming appends only here → **zero changes to
  the streaming path**.
- `session->metadata.branches` — NEW cJSON subtree:
  ```json
  {
    "list": [
      { "id": "br_<ts>-<rand>", "created_at": "...",
        "anchor_message_id": "<fork-group message id>",
        "messages": [ /* full chain snapshot root→leaf */ ] }
    ]
  }
  ```
- Exactly one chain lives in `messages`; every other chain is a snapshot
  record in `metadata.branches.list`. Old sessions without `branches` →
  single branch, count 1, no pills (read paths must tolerate absence).
- Stored in the existing encrypted `metadata_encrypted` blob → survives
  restarts and password migration untouched.

### Message identity on fork

When forking at message m (edit):

- Old chain keeps m untouched (original id, original content, original
  `fork_group_id`).
- New chain: messages `[0..m)` are kept **as-is** (same ids — shared
  prefix), m gets **fresh `id`** + **fresh `fork_group_id`**, content
  replaced (edit) or unchanged (regenerate); the tail after m is dropped
  from the live array (it lives on in the snapshot record).
- The pill renders under whichever chain-local message carries the
  fork_group: the old chain's original m or the new chain's edited m.

## 3. Backend changes

### 3.1 Serialization — complete the id plumbing

`ws_add_message_to_json` (src/server/routes/routes.c:13-48, used by both
REST session load and WS history) must additionally emit, when non-NULL:
`id`, `parent_id`, `fork_group_id`. Mirror the existing pattern at
src/agent/message.c:235-236 (emit only when present, keep legacy blobs
shape-equivalent).

`messages_to_json_array` (src/agent/message.c:197) already persists `id`;
add `parent_id` and `fork_group_id` the same way. Deserialize both in
src/session/session.c (around line 154-172, same partial-fail handling).

### 3.2 Session manager API (src/session/session_manager.c / .h)

- `session_manager_fork_branch(sm, session_id, index, new_content,
  out_branch_id)`:
  1. Lock; load session.
  2. Resolve the fork point by `message_id` when provided (scan loaded
     messages for id match), else by `index` (bounds-checked).
  3. Snapshot the full chain into a new record
     `{id, created_at, anchor_message_id, messages}` (deep copy via
     `messages_to_json_array` + deserialize, or `message_copy` loop).
  4. Truncate live array at the fork point; mint fresh id + fork_group_id
     on the fork message; set content.
  5. Persist `metadata.branches` + messages atomically
     (`save_session_core_locked`). **All-or-nothing**: any allocation
     failure before commit leaves the session unchanged (see §6
     fault-injection).
- `session_manager_switch_branch(sm, session_id, branch_id)`:
  1. Lock; load session; find record.
  2. Snapshot current live chain → replace its own record (or append a new
     record) so no chain is lost.
  3. Pop target record; load its snapshot into `session->messages`.
  4. Persist; unlock.
- `session_manager_branch_info_alloc(sm, session_id)` → JSON array of
  `{message_id, count, active}` for every fork point on the active chain.
  Count = number of chains (records + live) containing a message with that
  `fork_group_id`; active = position of the live chain among them (chains
  ordered by `created_at`).

### 3.3 WS handler (src/server/routes/routes_ws.c)

- `edit` (line 477): replace truncate-history flow with fork:
  - Read `message_id` (new) + `index` (fallback) + `content`.
  - `session_manager_fork_branch(...)`; on success, swap the agent's
    in-memory messages to the truncated chain (reuse the J2 system-prefix
    offset logic and the existing truncation at lines 498-507), run the
    stream as today, and on `done` emit `branch_info` with the new fork
    point.
  - `session_manager_truncate_history` remains for the fork's internal
    truncation; no other callers change.
- NEW message type `branch_switch {session_id, branch_id}`:
  - `session_manager_switch_branch(...)`; rebuild agent context from the
    new live chain (mirror the selectSession `message_copy` block,
    routes_ws.c:875-903); reply with the existing `history` event +
    `branch_info`.
- NEW frame `branch_info`:
  `{"type":"branch_info","branches":[{message_id,count,active},...]}`
  emitted: after history load on connect, after session select, after
  `edit`, after `regenerate`, after `branch_switch`.
- NEW message type `regenerate {session_id, index?, message_id?}` — same
  path as `edit` but content is left unchanged (fork + re-run).

### 3.4 REST session load

`GET /api/sessions/:id` response messages already flow through
`ws_add_message_to_json` (routes_session.c:199) → ids arrive after 3.1. Add
`branches` metadata (count + active position + fork points) so the pill
renders correctly on reload; the FE can also use `branch_info` if it loads
via WS history.

## 4. Frontend changes

### 4.1 Types & context (ChatContext.ts, types/index.ts)

- `Message` gains `id?`, `parent_id?`, `fork_group_id?`.
- New context state: `branchInfo: Array<{ messageId: string; count: number;
  active: number }>` (keyed by the message id as present in the active
  chain), plus `switchBranch(messageId, direction: -1 | 1)`.
- `editMessage` keeps its signature; `retryMessage` (dead code,
  ChatProvider.tsx:909) is removed and replaced by `regenerateMessage`.

### 4.2 ChatProvider.tsx

- Populate `msg.id`/`parent_id`/`fork_group_id` in the `history` and
  `loadSession` paths (they'll now arrive from the server).
- Handle `branch_info` frames → `setBranchInfo(...)`.
- `editMessage`: unchanged payload (index/content/session_id) **plus**
  `message_id` when the message has an id (now populated).
- `regenerateMessage(index)`: sends
  `{type:'regenerate', index, message_id}`; clears the local tail after the
  edited message and sets streaming, mirroring the edit flow.
- `switchBranch(messageId, direction)`: sends
  `{type:'branch_switch', session_id, branch_id}` — branch_id resolved
  client-side from `branchInfo` (target = active+direction) or
  server-returned list; the `history` reply re-renders the chain.
- The existing edit local-truncation (slice(0, index+1)) stays — it mirrors
  the active branch view.

### 4.3 BranchPill component (new, src/components/BranchPill.tsx)

- Rendered inside the message row (MessageList.tsx) below the message body
  when `branchInfo` contains an entry for that message's id and `count > 1`.
- Layout: `‹ 1/k ›` — left button disabled when `active === 1`, right
  disabled when `active === k` (no wrap-around).
- Clicking sends `switchBranch`.

### 4.4 Regenerate UI

- Last assistant message: "Regenerate" action (icon or text button) next to
  copy/actions on the final assistant message when not streaming →
  `regenerateMessage`.
- Edit box (existing "Regenerate (Ctrl+Enter)", MessageList.tsx:499):
  keeps sending the fork with the original text (no edit) —
  `regenerateMessage` instead of `editMessage` when the text is unchanged.

## 5. Wire contract summary (new/changed)

| Direction | Frame | Payload |
|---|---|---|
| S→C | `history` | messages now include `id`, `parent_id`, `fork_group_id` |
| S→C | `branch_info` | `{branches:[{message_id,count,active}]}` |
| C→S | `edit` | `{type,index,content,session_id,message_id?}` (message_id now honored) |
| C→S | `regenerate` | `{type,index?,message_id?,session_id}` |
| C→S | `branch_switch` | `{type,session_id,branch_id}` |
| S→C | `history` (after switch) | full active chain, as today |

## 6. Tests (AGENTS.md §6, §8, §11)

### Backend (Check, tests/routes + tests/session)

1. `test_fork_creates_branch_record` — edit forks: snapshot record exists,
   live array truncated, fork message has fresh id/fork_group.
2. `test_fork_by_message_id_resolves_index` — id-anchored fork cuts at the
   right position even when indices diverge (merged-UI simulation).
3. `test_fork_unknown_message_id_falls_back_to_index`.
4. `test_regenerate_keeps_content` — same fork, content unchanged.
5. `test_switch_branch_swaps_live_and_agent_context` — live ⇄ snapshot
   swap; agent messages rebuilt from the new chain.
6. `test_branch_info_counts_multifork_chain` — two fork points on one
   chain → two entries with correct count/active.
7. `test_old_session_without_branches_reports_single` — no
   `metadata.branches` → count 1, empty branch_info.
8. `test_fork_allocation_failure_leaves_session_unchanged` —
   fault-injection (REGISTRY_TEST-style guard): snapshot allocation fails →
   no record committed, live array untouched, error surfaced. This is the
   multi-allocation commit path per AGENTS.md §11.
9. Regression: existing `edit` tests updated from truncate semantics to
   fork semantics; demonstrate fail-on-old/pass-on-new via `git stash`.

### Frontend (vitest)

10. Pill hidden when count ≤ 1; visible with `‹ 1/2 ›` at count 2.
11. Arrows disabled at bounds (left at active=1, right at active=k).
12. Click `›` sends `branch_switch` with the next branch_id.
13. `branch_info` frame updates the pill counter/active.
14. Regenerate button on last assistant message sends `regenerate`.
15. Edit payload includes `message_id` when the message has an id.
16. Edit without id falls back to index-only payload (no `message_id` key).

### Fuzz

- Session deserialization fuzz target (fuzz_session_deserialize) already
  covers message parsing; extend input corpus to include `parent_id` /
  `fork_group_id` variants. No new target required.

## 7. Verification steps

1. `nix develop -c cmake --build build` + `ctest` — all suites green under
   ASan+UBSan (gcc), then `build-clang` (clang 21).
2. FE: `npx tsc -b`, `npm run lint`, `npm run test:run`.
3. Live smoke test: two browser sessions, edit + regenerate + switch,
   reload to confirm persistence; old-session compatibility (pre-branch
   DB) shows no pill.
4. Update FRONTEND_BACKEND_DISCREPANCIES.md: #9 → resolved by this
   feature; add the new wire frames to the "verified as consistent" list.

## 8. Open questions (all answered)

- Regenerate scope: last assistant message + any message via edit box. ✅
- Pill arrows: disabled at bounds, no wrap. ✅
- Pill label: `‹ 1/k ›` only, no branch names. ✅

## 9. Amendments (2026-08-08 — post-implementation fixes from live testing)

Live smoke testing (edit → switch → switch-back) surfaced three defects;
all fixed and covered by regression tests:

1. **Switch-back data loss** (worst bug). `branch_record_snapshot_live`
   replaced records by `anchor_message_id` — and sibling chains of one fork
   share the fork point's id, so switching back could overwrite the record
   of the chain the user had just left (history reverted to the unedited
   content, pill collapsed to count 1). Fix: records are now **append-only**,
   never replaced by anchor; `session_manager_switch_branch` additionally
   treats a switch to the already-live chain as a no-op (comparing
   serialized message arrays) so no phantom duplicate record is appended.
   Regression test: `test_switch_away_and_back_preserves_branch_data`.
2. **Wrong active position right after a fork** (`active:1` instead of 2).
   Chain timestamps were second-granularity (`%Y-%m-%dT%H:%M:%S`), so the
   snapshot record and the new live chain shared a `created_at` and strcmp
   ordering won arbitrarily. Fix: `branch_now_iso` now emits
   `%Y-%m-%dT%H:%M:%S.%3N`. Regression test:
   `test_fork_same_second_orders_active` (no sleeps, must be deterministic
   without them).
3. **Pill on the wrong message**. The frontend adopted the fork identity
   onto the last *assistant* message, so editing a user message put the
   pill on the following AI reply. Fix:
   - `done` after a fork now carries `fork_role` (the fork point's role:
     `user` for an edit, `assistant` for a regenerate) and the frontend
     adopts the fresh `fork_message_id`/`fork_group_id` onto the message
     with that role (`ChatProvider.tsx` done handler; for a user fork the
     content is left untouched — the edited text was applied at submit).
   - **Regenerate semantics changed**: the fork point is now the assistant
     message being regenerated (previously the backend stepped one message
     back into the user turn, so the pill landed on the user message).
     `ws_run_fork` (routes_ws.c) in regenerate mode truncates the agent
     context at the fork point WITHOUT appending the minted fork copy (the
     run would otherwise leave it as a ghost after the wholesale
     `agent_save_session`), then calls the new
     `session_manager_tag_message(sm, session_id, index, fork_group_id)`
     which re-applies the fork group + a minted id to the fresh response.
     The frontend's `regenerateMessage` slices to `index` (dropping the old
     assistant message) so the streamed chunks land in a fresh message.
   - `session_manager_fork_branch` re-forks now JOIN the fork point's
     existing `fork_group_id` instead of minting a new one (re-editing the
     same message keeps one pill covering the whole family instead of
     orphaning the earlier fork's chains). Regression tests:
     `test_refork_joins_existing_group`, `test_tag_message_marks_fork_point`,
     `test_tag_message_oom_leaves_session_unchanged`.
4. Test counts moved: session_manager suite 30 → 35 checks; frontend
   68 → 71 tests (3 new placement/persistence tests). Backend 40/40 ctest
   green under ASan+UBSan+LSan; FE tsc clean, lint 0 errors.
