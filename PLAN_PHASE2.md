# Phase 2 Plan — Completing the Architecture

**31 tasks** across 5 waves, ordered by dependency. Each wave builds on the ones before it.

---

## Wave 2A: Foundation Fixes

### 1. Single `agent_sessions` table
- **What**: Refactor `session_manager` from per-file SQLite databases to one database with an `agent_sessions` table, matching ARCHITECTURE.md §1.7 schema. Add process-wide write lock (mutex). Set `journal_mode=DELETE`, `synchronous=FULL` on connection.
- **Why**: Prerequisite for `sqlite_query`, `sqlite_schema`, export/import, cross-session queries. Fixes design divergence.
- **Files**: `session/session_manager.c/.h`, `session/session.c/.h`, `main.c`
- **Risks**: Destructive change — all existing `.session` files become obsolete. Need migration path or reset.
- **Depends on**: Nothing
- **Effort**: Large (3-4 days)

### 2. SQLite-backed rate limiter
- **What**: Replace in-memory rate limiter with SQLite-backed version that persists across restarts. Implement per-IP windows with atomic transactions, plus global unlock-failure counter with exponential backoff.
- **Why**: ARCHITECTURE §1.16 spec; prevents bypass-by-restart.
- **Files**: `utils/rate_limiter.c/.h`, `main.c`
- **Depends on**: #1 (uses same SQLite connection)
- **Effort**: Medium (1-2 days)

### 3. Wire metrics into execution
- **What**: Record tool execution latency/success/failure via metrics counters/histograms. Record LLM call duration and token counts. Track active WS connections. All exposed at `/api/metrics` (already exists).
- **Why**: Observability; ARCHITECTURE §1.16.
- **Files**: `utils/metrics.c/.h`, `agent/agent.c`, `tools/registry.c`, `server/server.c`
- **Depends on**: Nothing
- **Effort**: Small (1 day)

### 4. Mid-stream cancel fix
- **What**: Check `agent->cancel_requested` flag inside `agent_llm_call()` during streaming and between tool-calling iterations. In Ollama provider, abort the libcurl transfer on cancel.
- **Why**: ARCHITECTURE §1.2 says "cancel signal can be raised mid-stream". Currently the flag exists but is never checked during LLM calls.
- **Files**: `agent/agent.c`, `llm/ollama.c`
- **Depends on**: Nothing
- **Effort**: Small (1 day)

---

## Wave 2B: Tools & Providers

### 5. web_search — real search provider
- **What**: Implement `SearchProvider` interface with three backends: **Brave** (API), **DuckDuckGo** (scrape), **Tavily** (API). Config selects default provider and stores API keys. libcurl-based. Tool parses results and returns formatted text.
- **Files**: `tools/web_search.c/.h`, `tools/search_provider.h`, `tools/search_brave.c`, `tools/search_duckduckgo.c`, `tools/search_tavily.c`, `config/config.c`
- **Depends on**: Nothing (self-contained)
- **Effort**: Medium (2-3 days)

### 6. LMStudio provider
- **What**: Implement OpenAI-compatible provider (`/v1/chat/completions`, port 1234, streaming via SSE). `chat()`, `chat_streaming()`, `extract_structured()` (when #28 is done). Register in factory.
- **Files**: `llm/lmstudio.c`, `llm/factory.c`
- **Depends on**: Nothing (self-contained)
- **Effort**: Medium (1-2 days)

### 7. Core missing tools (9 tools)
- **What**: Implement and register:
  | Tool | Description | Key detail |
  |---|---|---|
  | `memory` | Persistent key-value user facts via SQLite | Uses session DB, CRUD operations |
  | `notes` | Personal markdown notes in notes directory | File I/O in configured notes dir |
  | `python_execute` | Run Python in subprocess with timeout | Fork/exec like bash tool, 30s default timeout |
  | `rest_api` | Arbitrary HTTP (GET/POST/PUT/DELETE/PATCH) | libcurl, JSON body, header passthrough |
  | `git` | Git operations (status/diff/log/add/commit/push/pull/branch/stash) | Subprocess git commands, 60s timeout |
  | `sqlite_query` | Read-only SQL against session DB | Parses SELECT, blocks writes, returns JSON rows |
  | `sqlite_schema` | Get table schemas from session DB | `PRAGMA table_info()` for each table |
  | `ask_user` | Human-in-the-loop prompt | Returns approval_response-style async wait via WS |
  | `replace_in_file` | Targeted search-and-replace in files | Read file, apply replacement, write back |
- **Files**: `tools/{memory,notes,python_execute,rest_api,git,sqlite_query,sqlite_schema,ask_user,replace_in_file}.c/.h`, `tools/registry.c`, `CMakeLists.txt`
- **Depends on**: #1 (sqlite_query/sqlite_schema need single DB); #14 (ask_user needs WS disconnect cleanup)
- **Effort**: Large (4-6 days) — can parallelize across tools

### 8. Advanced tools (5 tools)
- **What**: Implement and register:
  | Tool | Description | Key detail |
  |---|---|---|
  | `semantic_search` | Vector similarity over stored documents | In-memory TF-IDF or bag-of-words (no external vector DB) |
  | `ingest_document` | Add documents to search index | Reads file, chunks, indexes |
  | `humanizer` | Rewrite text via LLM | Calls agent's LLM with rewrite prompt |
  | `deep_search` | Multi-step research: fetch → filter → summarize | Orchestrates web_fetch + web_search |
  | `delegate` | Spawn sub-agent for sub-tasks | Needs #29 (Sub-agents) |
- **Files**: `tools/{semantic_search,ingest_document,humanizer,deep_search,delegate}.c/.h`, `tools/registry.c`, `CMakeLists.txt`
- **Depends on**: #5 (deep_search), #7 (delegate), #29 (sub-agents)
- **Effort**: Medium (2-3 days)

---

## Wave 2C: WebSocket & API Completion

### 9. WS edit message flow
- **What**: Handle `edit` type in `ws_chat_on_message`: load session from DB, find target message (by `message_id` or by index skipping system/tool), truncate history, deserialize into agent memory, re-run agent with new prompt. Return stream as normal.
- **Files**: `server/routes.c`, `agent/agent.c`
- **Depends on**: #1 (single DB makes session loading simpler)
- **Effort**: Medium (1-2 days)

### 10. WS session_start + title_updated events
- **What**: Emit `{"type":"session_start","session_id"}` at the start of each response. Wire title generation (#26) to emit `{"type":"title_updated","session_id","title"}`.
- **Files**: `server/routes.c`, `agent/agent.c`
- **Depends on**: #26
- **Effort**: Small (0.5 day)

### 11. WS ping/pong keepalive
- **What**: 15-second timer in server event loop. Send WS ping frame to each connected client. Close connection if pong not received within timeout.
- **Files**: `server/websocket.c/.h`, `server/server.c`
- **Depends on**: Nothing
- **Effort**: Small (0.5 day)

### 12. WS message queue
- **What**: Buffer messages received before WS `ready` event. Flush in order on ready. Discard on disconnect.
- **Files**: `server/routes.c`
- **Depends on**: Nothing
- **Effort**: Small (0.5 day)

### 13. WS stale event protection
- **What**: Track `active_session_id` per connection. Discard inbound events whose `session_id` doesn't match active session.
- **Files**: `server/routes.c`
- **Depends on**: Nothing
- **Effort**: Small (0.5 day)

### 14. WS disconnect cleanup
- **What**: On WebSocket close: cancel any pending approval (mark as denied), set cancel signal on streaming task, cancel background title generation.
- **Files**: `server/routes.c`, `agent/agent.c`
- **Depends on**: Nothing
- **Effort**: Small (1 day)

### 15. Missing REST endpoints (7 endpoints)
- **What**: Implement:
  | Endpoint | Method | Purpose |
  |---|---|---|
  | `/api/change-password` | POST | Full re-encryption, crash-safe (#1 makes this easier) |
  | `/api/models?provider=` | GET | List models from provider API, 60s cache |
  | `/api/preferences` | GET/POST | JSON file in data directory |
  | `/api/sessions/{id}/export` | GET | Export session as JSON |
  | `/api/sessions/import` | POST | Import session from JSON (reject duplicates) |
  | `/api/health/detailed` | GET | DB status, session count, uptime |
  | `/api/review` | GET | Review recommendations for UI hints |
- **Files**: `server/routes.c`, `session/session_manager.c/.h`, `utils/preferences.c/.h`, `llm/models_cache.c/.h`
- **Depends on**: #1 (export/import, change-password)
- **Effort**: Medium (2-3 days)

### 16. CLI chat commands
- **What**: Implement `/save`, `/load`, `/model`, `/undo`, `/redo`, `/clear`, `/help` in the CLI REPL loop.
  - `/save <name>` — save current session
  - `/load <id>` — load session by ID
  - `/model <name>` — switch model
  - `/undo` — undo last file change
  - `/redo` — redo last undone file change
  - `/clear` — clear screen
  - `/help` — list commands
- **Files**: `main.c`
- **Depends on**: #1 (save/load)
- **Effort**: Small (1 day)

### 17. Chat mode
- **What**: Implement the `--chat` flag startup path: lightweight REPL with no session management, single conversation, prints assistant responses to stdout.
- **Files**: `main.c`
- **Depends on**: Nothing
- **Effort**: Small (0.5 day)

---

## Wave 2D: Safety & Context Management

### 18. Safety: 25 dangerous patterns
- **What**: Expand from 6 hardcoded patterns to 25 regex patterns covering: `rm -rf /`, fork bombs, download-and-execute, disk writes, `chmod 777`, `sudo rm`, `dd if=/dev/zero`, `:(){ :|:& };:`, `wget ... | sh`, `curl ... | bash`, `mkfs`, `mkswap`, `dd if=/dev/urandom`, `shred`, `> /dev/sda`, `dd if=/dev/zero`, `pv < /dev/sda`, `debugfs`, `hdparm`, `mount -o loop`, `losetup`, `parted`, `fdisk`, `cfdisk`, `sfdisk`. Also add command parsing (split on `;`, `&&`, `||`, `|`) to check each sub-command.
- **Files**: `safety/safety.c/.h`
- **Depends on**: Nothing
- **Effort**: Medium (1-2 days)

### 19. Safety: config population
- **What**: Load `allowed_commands`, `blocked_commands`, `allowed_extensions`, `blocked_extensions`, `allowed_domains`, `require_approval_for`, `audit_log_path`, `read_requires_approval`, `read_size_threshold` from config into `SafetyConfig` struct.
- **Files**: `safety/safety.c`, `config/config.c`
- **Depends on**: Nothing
- **Effort**: Small (1 day)

### 20. Safety: destructive keyword detection
- **What**: Scan bash commands for keywords like "delete", "destroy", "format", "mkfs", "drop", "truncate", "shred", "wipe". Flag with warning but still execute (unlike dangerous patterns which block).
- **Files**: `safety/safety.c/.h`
- **Depends on**: #18
- **Effort**: Small (0.5 day)

### 21. Safety: audit logging
- **What**: Log all approval requests, approvals, and denials to configurable `audit_log_path`. JSON lines format with timestamp, tool name, arguments, user IP, outcome.
- **Files**: `safety/safety.c/.h`
- **Depends on**: #19
- **Effort**: Small (0.5 day)

### 22. Context window: full smart-select
- **What**: Implement full `smart_select` algorithm from ARCHITECTURE §1.3: priority 1 = system messages, priority 2 = tool interactions (keep pairs), priority 3 = recent messages (40% of budget), priority 4 = evenly sampled remainder. Handle orphaned tool results (include parent message).
- **Files**: `agent/context.c`
- **Depends on**: Nothing
- **Effort**: Medium (1-2 days)

### 23. Context window: token trimming
- **What**: Implement `trim_messages_by_tokens(messages, max_tokens)`: reverse-iterate keeping most recent messages within token budget. Handle orphaned tool results by including their parent assistant/user message.
- **Files**: `agent/context.c`
- **Depends on**: Nothing
- **Effort**: Medium (1-2 days)

### 24. Context window: sanitize_json
- **What**: `sanitize_json(str)`: strip markdown code fences (```json ... ```), remove trailing commas before `]` and `}`, trim whitespace.
- **Files**: `agent/context.c`
- **Depends on**: Nothing
- **Effort**: Small (0.5 day)

### 25. Background summarization
- **What**: After each agent run completes, if context window dropped messages, call LLM to summarize them via `summarize_old_messages`. Store summary in `session.metadata.summary`. On next run, inject summary into system prompt: "Previous conversation summary: ...".
- **Files**: `agent/agent.c`, `agent/context.c`, `session/session.h`
- **Depends on**: #22, #23
- **Effort**: Medium (2-3 days)

### 26. Title generation
- **What**: After first user message in a session, call LLM async to generate a short title (≤5 words). Store in `session.title`. Emit `{"type":"title_updated"}` via WebSocket if connected.
- **Files**: `agent/agent.c`, `server/routes.c`
- **Depends on**: #10
- **Effort**: Small (1 day)

---

## Wave 2E: Advanced Systems

### 27. Callback system
- **What**: Define callback interface with hooks:
  - `on_run_start(run_id, prompt)` / `on_run_end(run_id, response)` / `on_run_error(run_id, error)`
  - `on_llm_start(run_id, messages)` / `on_llm_end(run_id, response)`
  - `on_tool_start(run_id, tool_name, kwargs)` / `on_tool_end(run_id, tool_name, result)` / `on_tool_error(run_id, tool_name, error)`
  - Implement `CallbackManager` that broadcasts to all registered callbacks.
  - Wire into agent loop.
- **Files**: `utils/callbacks.c/.h`, `agent/agent.c`
- **Depends on**: Nothing
- **Effort**: Medium (2-3 days)

### 28. Structured extraction
- **What**: Add `extract_structured(messages, response_model, temperature)` to provider interface. For Ollama: use `/api/chat` with `format` parameter (JSON mode). For LMStudio: use response_format with JSON schema.
- **Files**: `llm/provider.h`, `llm/ollama.c`, `llm/lmstudio.c`
- **Depends on**: #6 (LMStudio)
- **Effort**: Medium (1-2 days)

### 29. Sub-agents / DelegateTool
- **What**: Register sub-agents from config (name, description, model, tools, system_prompt). `DelegateTool` spawns a transient `Agent` with no session, shares root agent's LLM, runs a sub-task, returns result as string. Cleans up after completion.
- **Files**: `tools/delegate.c/.h`, `agent/agent.c`, `config/config.c`
- **Depends on**: #27
- **Effort**: Medium (2-3 days)

### 30. Memory system (facts)
- **What**: `MemoryTool` with key-value storage in the session database (separate `user_memory` table). CRUD operations. On agent start, `load_persistent_memory()` retrieves all stored facts and injects into system prompt: "User facts: {key: value, ...}".
- **Files**: `tools/memory.c/.h`, `session/memory.c/.h`, `agent/agent.c`
- **Depends on**: #1 (session DB)
- **Effort**: Medium (1-2 days)

---

## Dependency Graph (simplified)

```
1 (single DB) ───┬── 2 (SQLite rate limiter)
                  ├── 7 (sqlite_query/schema)
                  ├── 9 (WS edit flow)
                  ├── 15 (export/import/change-password)
                  ├── 16 (CLI save/load)
                  └── 30 (memory facts)
                        
6 (LMStudio) ──── 28 (structured extraction)

5 (web_search) ── 8 (deep_search)

29 (sub-agents) ─ 8 (delegate)

10 (title event) ─ 26 (title gen) ─ 25 (summarization)
22 (smart-select) ─ 25 (summarization) ─ 23 (token trim)

27 (callbacks) ── 29 (sub-agents)
```

Independent work that can be parallelized:
- **2A**: #3 + #4 can run in parallel with #1
- **2B**: #5 + #6 + #7 (non-DB tools like python_execute, notes, rest_api, git) can run in parallel
- **2C**: #11 + #12 + #13 + #14 + #17 can run in parallel
- **2D**: #18 + #19 + #22 + #23 + #24 can run in parallel
- **2E**: #27 + #30 can run in parallel

---

## Effort Summary

| Wave | Tasks | Est. days |
|------|-------|-----------|
| 2A Foundation | 4 tasks | 5-8 |
| 2B Tools & Providers | 4 tasks (14 tools) | 9-14 |
| 2C WebSocket & API | 9 tasks | 7-10 |
| 2D Safety & Context | 9 tasks | 8-11 |
| 2E Advanced Systems | 4 tasks | 6-9 |
| **Total** | **31 tasks** | **35-52 days** |
