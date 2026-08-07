# Frontend ↔ Backend Discrepancy Scan

Scan date: 2026-08-01
Method: traced every REST/WebSocket contract from `frontend/src` against the handlers in `src/server`, `src/agent`, `src/llm`, `src/tools`.

## CRITICAL — core chat flow is broken

### 1. WebSocket auth is impossible from a browser — **FIXED**
- Old behavior: `/ws/chat` upgrade required the `X-Unlock-Token` **header** — `src/server/server.c:280`, checked via `middleware_check_unlock` (`src/server/middleware.c:50`, header-only, no query-param fallback). The frontend's `new WebSocket('/ws/chat')` (`frontend/src/context/ChatProvider.tsx:103`) could not set custom headers, so the upgrade was always rejected with 401.
- Fix: the token now travels in the `Sec-WebSocket-Protocol` subprotocol value, which browsers can set via the WebSocket API. The gate (`middleware_check_unlock_ws`) accepts the protocol value **or** the header (non-browser clients keep working), and `ws_do_handshake` echoes the protocol in the 101 response per RFC 6455.
- Files: `src/server/middleware.c`/`.h` (`middleware_protocol_token`, `middleware_check_unlock_protocol`, `middleware_check_unlock_ws`), `src/server/server.c`, `src/server/websocket.c`/`.h` (`ws_handshake_response_alloc`), `frontend/src/context/ChatProvider.tsx:103`.
- Verified: old binary returns 401 for a protocol-only upgrade; fixed binary returns 101 (echoed protocol) with the first frame flowing, and still 401 for missing/wrong tokens. 25 new Check tests (20 middleware + 5 handshake-response) pass under ASan+UBSan; all 29 suites green.
- Remaining caveat (unchanged): the token crosses the wire in plaintext on every REST call too — this fix removes persistent copies (URLs/history/referrer/logs), not wire visibility. TLS would be the real fix.

### 2. `ask_user` tool hangs chat indefinitely — **FIXED**
- Old behavior: `ws_ask_user_cb` (`src/server/routes/routes_ws.c:557`) emitted `{"type":"ask_user","question":...}` and blocked in an unbounded `uv_run` spin-loop until `{"type":"ask_user_response","answer":...}` arrived; the frontend had no `ask_user` handler and never sent a response — any agent call of `ask_user` (`src/tools/tool_ask_user.c:77`) froze the connection forever.
- Fix: the frontend now mirrors the approval flow — a `case 'ask_user'` in `ChatProvider.tsx` shows an `AskUserDialog` (text input + Send/Cancel), and `resolveAskUser` sends `ask_user_response`. Backend-side hardening: the spin-loop is bounded by a configurable `ask_user_timeout` (seconds, default 60; `config.conf.example`), and on timeout / connection loss / `stop` it returns `"(user did not respond)"` instead of NULL — previously NULL fell through to the CLI stdin fallback (`tool_ask_user.c:36` `getline`), which would hang a web-mode server that had no terminal reader.
- Files: `src/server/routes/routes_ws.c` (`WSChatCtx.ask_user_timeout`, `routes_ws_chat_init`, `ws_ask_user_cb`), `frontend/src/context/ChatProvider.tsx`, `frontend/src/components/AskUserDialog.tsx`, `frontend/src/types/index.ts`, `frontend/src/context/ChatContext.ts`, `config.conf.example`.
- Verified: 2 new Check tests (`test_ask_user_cb_times_out_without_response`, `test_ask_user_cb_default_timeout_never_nulls`) exercise the timeout deterministically via stubbed `uv_now`/`uv_run`; all 29 suites green under ASan+UBSan; frontend typecheck/lint/tests clean.

## HIGH — features that silently don't work

### 3. Provider selection is decorative — **FIXED**
- Old behavior: the WS config handshake read `provider` but never applied it (`src/server/routes/routes_ws.c:260-266` — only `agent_set_model` was called; the provider was baked from `config.conf` at startup and there was no `agent_set_provider` anywhere, so the sidebar's provider choice changed nothing on the wire).
- Fix: new `agent_set_provider` (`src/agent/agent.c`/`agent.h`) rebuilds the live `LLMProvider` via `get_provider`, swap-safe — on failure the old provider is kept and the connection stays usable. The handshake now applies the provider before the model, maps the FE spelling `lm_studio` → canonical `lmstudio`, and sends a `{"type":"error","content":"provider switch failed: <name>"}` frame when the factory can't create the provider (e.g. `openai`/`anthropic`, which the backend does not implement).
- Files: `src/agent/agent.c`/`agent.h` (`agent_set_provider`, `Agent.provider_name`), `src/server/routes/routes_ws.c` (`WSChatCtx.base_url/num_ctx/keep_alive_secs`, handshake).
- Verified: 5 new Check tests for `agent_set_provider` (swap, same-name no-op, failure keeps old provider, NULL args, model untouched) + 4 new handshake tests (alias mapping + params passed, error frame + ready still sent, same-provider, provider-less config rejected as missing type); 30 suites green under ASan+UBSan. Live before/after on the wire: old binary silently answers `ready` to a config switching to `openai`; new binary answers `error: provider switch failed: openai` then `ready`; `lm_studio` is accepted without error.
- Update (same pass as #4): the OpenAI-compatible provider is now canonical **`openai`** — `src/llm/lmstudio.c` was renamed to `src/llm/openai.c` and the FE list trimmed to `['ollama','openai','anthropic']`. The `openai` provider's endpoint is `openai.base_url` (default `https://api.openai.com`; LM Studio etc. point it at a local server). `lmstudio`/`lm_studio` remain aliases for back-compat. No API-key support yet — real OpenAI rejects with 401 until keys land.
- Caveat (unchanged): the `delegate` tool's provider comes from `registry_set_delegate_config` at startup, so a mid-session provider switch does not affect it.

### 4. `GET /api/models` ignores `?provider=` — **FIXED**
- Old behavior: the backend always queried `http://localhost:11434/api/tags` (`src/server/routes/routes_general.c:180`), so switching provider still listed Ollama models.
- Fix: `handle_models` now reads `provider` from the query string. `ollama` → `<ollama.base_url>/api/tags` (`.models[].name`); `openai` → `<openai.base_url>/v1/models` (`.data[].id`, the OpenAI-compatible shape LM Studio/vLLM also serve); `lm_studio` normalized to `openai`; unknown providers return an empty list without any HTTP call. `load_agent_config` likewise resolves `agent.base_url` per provider (`openai.base_url` default `https://api.openai.com`, `ollama.base_url` default `http://localhost:11434`).
- Files: `src/server/routes/routes_general.c`, `src/main.c`, `config.conf.example` (`[openai]` section).
- Verified: 6 new Check tests (ollama default URL, explicit query, openai URL + `.data[].id` parsing, `lm_studio` alias, custom `openai.base_url` from a real conf file, unknown provider → empty without curl) + 1 updated; 30 suites green under ASan+UBSan. Live before/after with two fake model servers: old binary returns the Ollama list for every provider; new binary returns each provider's own list.

### 5. Preferences endpoint is a stub — **RESOLVED (routes removed)**
- Old behavior: `GET /api/preferences` returned `{"preferences":{}}` while the FE read top-level `model`/`provider` → always `undefined` → model/provider reset on every reload; `POST /api/preferences` discarded the body (`(void)req`).
- Resolution: the endpoints were removed rather than implemented — `handle_preferences_get`/`handle_preferences_set` deleted from `src/server/routes/routes_general.c`/`.h` and the routes dropped from the table (`src/server/routes/routes.c`), so there is no longer a contract to diverge. The FE persists model/provider/models/effort to `localStorage` (`echo-ai-chat-preferences`, `frontend/src/context/ChatProvider.tsx:26-42`) as the single source of truth; `getPreferences`/`setPreferences` were deleted from `frontend/src/api/client.ts`. Per-browser persistence only — no cross-device sync.
- Verified: `test_routes_general` preferences tests removed; 29 suites green under ASan+UBSan; FE typecheck/lint/tests clean.

### 6. `change-password` never verifies the current password — **FIXED**
- Old behavior: the backend read only `new_password` (`src/server/routes/routes_auth.c`), so anyone holding the unlock token (the UI being open, or a lifted localStorage token) could re-encrypt the DB to a password of their choice without knowing the current one; `confirm` was also unverified server-side.
- Fix: `handle_change_password` now requires and verifies `current_password` via the same `session_manager_create_ex` check the unlock flow uses (AUTH_FAILED → 401 "current password is incorrect", recorded against the same rate-limiter bucket as `/api/unlock` — 5 attempts/20s → 429), rejects `confirm` mismatches with 400, and aligns the new-password minimum with the FE (8 chars, matching the dialog's error mapping). The throwaway verification manager is freed before any key material is touched; password buffers are zeroed after use. The FE already sent all three fields and rendered the error branch — no FE change needed.
- Verified: 6 new Check tests (missing current → 400, confirm mismatch → 400, wrong current → 401 + rate-failure recorded + `migration_change_password` not called, rate-limited → 429, storage failure → 500, success path still 200 + migration called) fail on the old handler (wrong current returned 200) and pass on the new one; 40 suites green under ASan+UBSan.

## MEDIUM

### 7. WS error frames use two different keys
- Backend sends both `{"type":"error","message":...}` ("invalid json", "stale session_id", "session not found") and `{"type":"error","content":...}` ("no response", "missing message content").
- Frontend only reads `data.content` (`frontend/src/context/ChatProvider.tsx:374`) → `message`-key errors render as generic "An error occurred".

### 8. Tool-call result wire shape mismatch on reload
- Backend serializes results as `tool_calls[].result_content` / `result_error` (`src/server/routes/routes.c:38-41`, `src/server/routes/routes_ws.c:86-89`).
- Frontend types/renderers read `tc.result.{content,error}` (`frontend/src/types/index.ts:24-27`, `frontend/src/components/MessageList.tsx:203`).
- Live results are reconstructed from `tool_end` events, but on `loadSession` / `history` reload the conversion only happens incidentally via role `tool` messages (`frontend/src/context/ChatProvider.tsx:22-33`) — multiple results collapse onto the first unresolved call; tool results can render as perpetually "running".

### 9. `edit` payload has a dead `message_id` field
- Frontend sends it (`frontend/src/context/ChatProvider.tsx:528`); backend parses only `index` / `content` (`src/server/routes/routes_ws.c:312-317`).

### 10. `session.enabled=false` mode is unusable from the FE
- `src/main.c:524` sets `unlock_token="noop"`; `handle_status` then reports `locked=1` unless that header matches (`src/server/routes/routes_general.c:20-22`), `/api/unlock` fails ("salt not found"), and the WS is blocked (issue 1). The UI can never get past the unlock screen.

## LOW / dead contract (harmless but divergent)

- **Backend-only, never called by FE**:
  - `POST /api/chat`
  - `GET /api/stream` (SSE — also runs with an empty prompt, `src/server/routes/routes_chat.c:134`)
  - `POST /api/sessions/import`
  - `PUT /api/sessions/`
  - `GET /api/health/detailed`
  - `GET /api/metrics`
  - `POST /api/undo`
  - `POST /api/redo`
- **FE-only client methods never used in the app**: `createSession` (sessions are created lazily server-side), `getConfig`, `healthCheck`.
- **Dead `StreamEvent` members** (`frontend/src/types/index.ts:45-58`): `'pong'` (ping is protocol-level, `src/server/websocket.c:36` — never a JSON event) and `'message'` (never sent by the backend).
- Backend never sends `timestamp` on messages; the FE only timestamps user messages client-side.

## Verified as consistent (no action)

- All REST paths and method pairs used by the FE exist in the backend route table (`src/server/routes/routes.c:50-74`).
- Session create/list/load/delete/rename request/response shapes.
- `unlock` / `setup` / `logout` / `status` / `health` request/response shapes.
- `X-Unlock-Token` header handling for axios REST calls, including `debug-export` (`frontend/src/components/Header.tsx:114`).
- WS events `ready` / `content` / `done` / `tool_start` / `tool_end` / `history` / `title_updated` / `session_start` / `approval_request` / `approval_response`, and `stop`.
- All 24 backend tool names match the FE tool labels (`frontend/src/components/MessageList.tsx:105-177`).
