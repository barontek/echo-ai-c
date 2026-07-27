# Echo AI — Language-Agnostic Architecture

## Overview

Echo AI is an agentic AI system with a **conversational frontend** and a **tool-using backend**. A user holds multi-turn conversations with an LLM that can invoke tools (run bash, read/write files, search the web, query databases, execute code, etc.) to accomplish tasks. Conversations are persisted as **sessions** in an encrypted SQLite database.

The system has two deployable surfaces:
- **Backend**: An HTTP + WebSocket server exposing REST endpoints and a WebSocket chat endpoint
- **Frontend**: A browser SPA that communicates with the backend via REST and WebSocket

---

## PART 1: BACKEND ARCHITECTURE

### 1.1 Entry Points

The system can start in three modes:
1. **CLI mode**: Interactive REPL with rich-rendered chat, supports chat commands (`/new`, `/save`, `/load`, `/model`, `/undo`, `/redo`, `/clear`, `/help`, `/exit`)
2. **Chat mode**: Lightweight interactive chat
3. **Web server**: HTTP server on port 8080 with full REST API + WebSocket

### 1.2 Core Agent Loop

The **Agent** is the central orchestrator. Its configuration specifies:
- Provider name (ollama, lm_studio)
- Model name
- Temperature (0.0–2.0)
- Timeout (seconds)
- Max iterations for the tool-calling loop (default 50)
- Max context messages (default 50) and max context chars (default 100K)
- System prompt
- Tool list
- Base URL (for local providers)
- Session enabled/disabled
- Session storage directory
- Parallel tool execution flag
- num_ctx (Ollama context window size)

**Lifecycle:**
1. User calls `run(user_input)` or `run_streaming(user_input, on_chunk)`
2. On first run, `load_persistent_memory()` loads stored user facts from a MemoryTool
3. The user message is appended to the internal `messages[]` list and saved to the session database
4. The agent enters a **tool-calling loop**:

```
for iteration in range(max_iterations):
    1. Prepare messages for the LLM:
       - Apply context window (trim by char count then message count, smart-select)
       - Inject system prompt + system context (OS, CWD, time)
       - Inject sub-agent descriptions if any are registered
       - Inject conversation summary from session metadata
       - Format as list of {role, content, tool_calls} dicts
    2. Call LLM (streaming if available + on_chunk provided, else non-streaming)
    3. If response has tool_calls:
       - Record assistant message with tool_calls in history
       - Execute each tool via tool runtime (sequentially or in parallel)
       - If any tool execution error, save error in history
       - On critical errors (execution_error, timeout), log warning
       - Append tool results as messages
       - Save session to database
    4. If response has no tool_calls:
       - Post-tool-call synthesis guard: if tools were executed and response is only <think> content, retry once
       - Append final assistant message
       - Return response text + updated messages

If max iterations exceeded: return error message
```

**Key behaviors:**
- **Background summarization**: When context window drops messages, those are stored as pending. After the run completes, a background task summarizes them via the LLM and inserts a system message with the summary.
- **Generation stop**: A cancel signal can be raised mid-stream. Partial content is preserved and returned.
- **Session-aware**: If session management is enabled, every user message and assistant response is persisted to the SQLite database in real-time.
- **Undo/Redo**: File changes are tracked via ChangeTracker with large-file spill-to-disk (>50KB). undo() and redo() reverse/restore file operations.
- **Sub-agents**: Registered sub-agents expose a DelegateTool that spawns a transient Agent (no session, sharing the root agent's LLM) to handle sub-tasks, returning the result as a string.
- **Title generation**: After a response, an async task generates a short session title (max 5 words) via the LLM.
- **Structured extraction**: extract_data(prompt, schema_model) calls the LLM's structured extraction to get typed output matching a schema.

### 1.3 Conversation Model

Data structures:
- **Message**: `{role: user|assistant|system|tool, content, id, tool_calls[], tool_call_id, tool_name, tool_arguments, error_category, timestamp, thinking}`
- Tool calls in assistant messages: `[{id, type: "function", function: {name, arguments: dict}}]`
- Tool result messages: `{role: "tool", content, tool_call_id, tool_name, tool_arguments, error_category}`

Key functions:
- `format_messages_for_llm(messages, system_prompt, sub_agents)`: Converts Message objects to LLM API format, injects system context (OS, CWD, time, sub-agent list)
- `apply_context_window(messages, max_messages, max_chars)`: Trims by token count first (70% of char budget), then by message count using smart selection (system > tool interactions > recent > evenly sampled)
- `smart_select(messages, keep_count)`: Preserves informative subset rather than FIFO truncation
- `trim_messages_by_tokens(messages, max_tokens)`: Reverse-iterates keeping most recent messages within token budget; handles orphaned tool results by including their parent assistant/user message
- `split_thinking_content(raw_content)`: Parses `<think>...</think>` tags from content, handles multiple blocks and unterminated trailing tags
- `summarize_old_messages(messages, llm)`: LLM-based summarization of dropped context
- `sanitize_json(str)`: Removes markdown code fences and trailing commas

### 1.4 LLM Provider Interface

Abstract interface every provider must implement:

```
LLMProvider:
    chat(messages: [{role, content}], tools?, temperature?) -> LLMResponse
    chat_streaming(messages, tools?, temperature?, on_chunk?) -> LLMResponse
    extract_structured(messages, response_model, temperature?) -> typed_object

LLMResponse = {content: str, thinking: str?, tool_calls: [LLMToolCall]}
LLMToolCall = {id: str, name: str, arguments: dict}
```

There is also a structural protocol for duck-typing compatibility.

**Two implementations:**
1. **OllamaProvider**: HTTP to `localhost:11434/api/chat`. Handles streaming via SSE. Keeps a connection pool. Supports num_ctx parameter. Normalizes arguments between string/dict formats.
2. **LMStudioProvider**: HTTP to `localhost:1234/v1/chat/completions` (OpenAI-compatible API). Supports streaming.

Provider selection by name: factory function `get_provider(name, model, api_key?, base_url?, timeout?, num_ctx?)`

### 1.5 Tool System

Base class:
```
Tool:
    name: str
    description: str
    parameters_model: Schema? (auto-generates JSON schema for LLM)
    schema -> function-calling format
    execute(**kwargs) -> ToolResult
    
ToolResult = {content: str, error: str?, error_category: str?, metadata: dict?}
```

**18 registered tools:**

| Name | Description |
|---|---|
| `bash` | Execute shell commands with allowlist, timeout, dangerous-pattern detection, and per-command safety rules |
| `read_file` | Read file contents (blocked extensions checked, path traversal checked, size limited) |
| `write_file` | Write content to files (same safety checks + change tracking for undo) |
| `list_dir` | List directory contents |
| `glob` | File pattern matching |
| `grep` | Content search within files |
| `web_fetch` | HTTP GET a URL, return text content (domain allowlist, size limit) |
| `web_search` | Web search via configurable provider (Brave, DuckDuckGo, Tavily) |
| `git` | Git operations (status, diff, log, add, commit, push, pull, branch, stash, etc.) with timeout |
| `memory` | Store/retrieve/delete persistent user facts (key-value storage in SQLite) |
| `notes` | Persistent personal notes (markdown files in a notes directory) |
| `python_execute` | Execute Python code in a subprocess with timeout |
| `rest_api` | Arbitrary HTTP requests (GET, POST, PUT, DELETE, PATCH) with JSON body support |
| `sqlite_query` | Read-only SQL queries against the session database |
| `sqlite_schema` | Get table schemas from the session database |
| `ask_user` | Prompt the user for information and wait for response (human-in-the-loop) |
| `semantic_search` | Vector similarity search over stored documents |
| `ingest_document` | Add documents to vector store for semantic search |
| `humanizer` | Rewrite text to be more natural/conversational using LLM |
| `deep_search` | Multi-step research: fetch -> filter -> summarize (calls web_fetch + web_search internally) |
| `delegate` | Spawn a sub-agent (lazily added when sub-agents are registered) |

Key design points:
- Every tool can declare a parameters model for automatic argument validation
- Tool execute() is async (allows parallel execution)
- Tool schemas are auto-generated from the model for LLM tool-calling
- Search providers plugin architecture: base class `SearchProvider` with `search(query, num_results)` and `get_limits()`

### 1.6 Tool Execution Runtime

When the LLM returns tool calls, they are processed:

1. **Validation**: Parses arguments JSON, validates against tool's parameters model
2. **Execution**: Calls `tool.execute(**validated_args)` - sequentially or in parallel
3. **Error handling**: Categorized errors: validation_error, policy_denied, execution_error, timeout, tool_not_found, file_not_found, permission_denied
4. **Change tracking**: If tool returns metadata with change info, records it for undo/redo
5. **Revert on failure**: If a tool fails mid-execution, changes for that specific tool call are rolled back
6. **Metrics**: Records tool execution latency and success/failure

### 1.7 Session Management

**Storage**: SQLite database with an ORM layer. One table `agent_sessions`.

**Schema**:
```
agent_sessions:
  id: TEXT PRIMARY KEY
  title_encrypted: BLOB (Fernet-encrypted title; nullable)
  title_generation_attempted: INTEGER (default 0)
  created_at: TEXT (ISO 8601 `YYYY-MM-DDTHH:MM:SS`, no timezone)
  messages_encrypted: BLOB (Fernet-encrypted JSON array of messages)
  metadata_encrypted: BLOB (Fernet-encrypted JSON object)
  events_encrypted: BLOB (Fernet-encrypted JSON array of events)
```

**Encryption**: Column-level transparent encryption. Uses Fernet (AES-128-CBC + HMAC-SHA256). Key derived from user password using Scrypt. Salt stored in a file literally named `salt` inside the data directory (`SALT_FILE = "salt"` in `src/session/session_manager.c`; same convention in `src/session/encryption.c`).

**Session** (in-memory domain object):
```
{
  id, title?, title_generation_attempted, created_at,
  messages: [{role, content, timestamp, thinking?, tool_calls?}],
  metadata: {summary?, checkpoints?},
  events: [{event_type, data, timestamp}]
}
```

**SessionManager** operations (as implemented in `src/session/session_manager.{c,h}`):
- `create_session(title)` -> creates in DB and returns the Session
- `load_session(id)` -> loads from DB into a fresh Session (returns NULL on miss/error)
- `save_session(session)` -> upserts to DB (acquires process-wide mutex)
- `delete_session(id)` -> removes from DB
- `list_sessions()` -> non-paginated listing of all sessions (titles decrypted in-memory)
- `add_message(session_id, role, content, tool_call_id?, tool_name?)` -> load-modify-save cycle
- `truncate_history(session_id, index)` -> removes all messages at and after index (load-modify-save)
- `purge_sessions(older_than_days)` -> bulk delete old sessions
- `export_session(id)` -> exports to JSON string
- `import_session(json_str)` -> imports from JSON string (rejects duplicate ids)
- `log_event(session_id, event_type, data)` -> appends to session event log (load-modify-save)

Not implemented (documented previously but absent from the C port): `save_checkpoint`, `purge_empty_sessions`, `get_history`, `close`, paginated list, in-memory title search.

**Migration system**: No schema migration machinery. The DB schema is fixed at table-creation time by `init_db` (CREATE TABLE IF NOT EXISTS); adding or renaming a column requires dropping the DB and re-creating it. Plaintext-title re-encryption is not performed (titles are encrypted at first write only).

**Concurrency**: A process-wide `pthread_mutex_t` serializes all writes against a single shared `sqlite3*` connection. No connection pooling.

**SQLite settings**: journal_mode=DELETE, synchronous=FULL for maximum durability.

**ChangeTracker**: Records file changes for undo/redo:
- Stores old/new content, with automatic spill-to-disk for content >50KB
- Per-tool_call_id tracking allows reverting all changes from a failed tool
- Per-session backup directories to prevent cross-session undo corruption

### 1.8 Database Encryption

Key derivation: **Scrypt** (N=2^18, r=8, p=1, 32-byte key) -> base64-urlsafe -> Fernet key.

**Salt format**:
- v1 (legacy): 16 raw random bytes, Scrypt N=2^14
- v2 (current): byte[0]=0x02, byte[1:17]=random bytes, Scrypt N=2^18

**Password resolution order** (see `encryption_resolve_password` in `src/session/encryption.c`):
1. Environment variable `ECHO_PASSWORD`
2. Plaintext file at `~/.config/echo-ai/password`
3. If neither: returns NULL; the caller (CLI/web bootstrap) is responsible for surfacing the error or prompting interactively

**First-run detection**: True only if the salt file is absent. (The DB file is not consulted, since a user may legitimately delete their DB while keeping their password/salt; in that case a fresh DB is created on the existing key.) Used to gate salt creation vs unlock.

**Atomic salt creation**: Uses exclusive-create open flags to prevent race conditions between processes.

**Change-password crash recovery**:
- Uses a `.changing_pwd` marker file (not SQLite PRAGMA user_version) as the crash signal
- Writes the old salt to `salt.old` BEFORE touching DB
- Re-encrypts all rows one at a time (no single-transaction guarantee; see `migration_change_password` in `src/session/migration.c`)
- On crash recovery at startup: `migration_check_and_recover` restores `salt.old` -> `salt` and removes the marker file; rows re-encrypted before the crash remain under the new key and may become unreadable
- Full crash-coverage table documented in the code

**Memory scrubbing**: Best-effort zeroing of password string after use. Environment variable removed after reading.

### 1.9 Safety System

**SafetyConfig**:
```
workspace, allowed_commands[], blocked_commands[], allow_network,
enable_domain_allowlist, allowed_domains[], max_file_size,
max_execution_time, require_approval_for[], approval_callback,
async_approval_callback, audit_log_path, read_requires_approval,
read_size_threshold
```

**SecurityValidator** checks:
1. **Path traversal**: Rejects `..` traversals and non-workspace absolute paths. Resolves symlinks.
2. **Blocked extensions**: .key, .pem, .env, .token, .password, .aws, .git/credentials, .netrc, .htpasswd, .crt, .p12, etc.
3. **Blocked paths**: /etc/passwd, /etc/shadow, /etc/sudoers, ~/.ssh, ~/.aws, .git/config, Windows system paths
4. **Command safety**: 25 regex patterns (rm -rf /, fork bombs, download-and-execute, disk writes, chmod 777, sudo rm, etc.)
5. **Command parsing**: Splits on `;`, `&&`, `||`, `|` to check each sub-command individually. Handles env-var prefix stripping. Uses glob matching (not regex) for command matching to prevent ReDoS.
6. **Network domain allowlist**: Blocks/allows URLs by domain pattern matching
7. **File size enforcement**: Hard limit on reads/writes
8. **Approval system**: Certain tools require user approval before executing (bash, write_file, memory, sqlite_query by default). Can be sync (stdin read with timeout) or async (WebSocket message to frontend with 120s timeout).
9. **Audit logging**: All approvals/denials logged to a file
10. **Destructive keyword detection**: For bash commands, detects "delete", "destroy", "rm -rf", "format", "mkfs", etc.

### 1.10 Web API Endpoints

**Application State**:
```
{
  agent: Agent?,
  fernet: Fernet?,
  fernet_key: bytes?,
  current_session_id: str?,
  message_history: [{role, content, timestamp}],
  active_tokens: {str}  // set of active unlock tokens
}
```

**Middleware stack** (applied in order):
1. Correlation ID header for structured logging
2. Bearer token auth (optional, configurable)
3. Rate limiting (global + per-IP, configurable requests/window)
4. Unlock token validation (custom header checked against active token set)
5. Request logging (method, path, duration)

**REST API Endpoints:**

| Method | Path | Auth | Description |
|---|---|---|---|
| GET | /api/status | None | Check lock/setup state |
| POST | /api/setup | None | First-run password creation. Returns token. |
| POST | /api/unlock | None | Password submission. Rate-limited (5/IP/min + 20 global/min). Returns token. |
| POST | /api/logout | Unlock token | Invalidates token. If no tokens remain, destroys agent = re-lock. |
| POST | /api/change-password | Unlock token + unlocked | Full re-encryption of all sessions. Crash-safe. |
| GET | /api/config | None* | Returns current provider, model, temperature, etc. |
| POST | /api/config | Unlock token | Change provider/model. Creates new Agent. |
| GET | /api/preferences | None | Get user preferences |
| POST | /api/preferences | None | Save user preferences |
| GET | /api/models?provider= | None | List available models for a provider (60s cache) |
| GET | /api/sessions | Unlock token | List all sessions |
| POST | /api/sessions | Unlock token | Create new session |
| GET | /api/sessions/{id} | Unlock token | Load session with filtered messages |
| DELETE | /api/sessions/{id} | Unlock token | Delete session |
| POST | /api/sessions/rename | Unlock token | Rename session |
| GET | /api/sessions/{id}/export | Unlock token | Export session as JSON |
| POST | /api/sessions/import | Unlock token | Import session from JSON |
| GET | /api/health | None | Health check |
| GET | /api/health/detailed | None | Detailed health |
| POST | /api/chat | Unlock token | Synchronous chat |
| POST | /chat | Unlock token | Alternative sync chat with session management |
| GET | /stream | Unlock token | SSE streaming chat |
| POST | /route | Unlock token | Semantic routing |
| GET | /api/review | None | Review recommendations for UI hints |
| WS | /ws/chat | Bearer token (optional) + unlocked | WebSocket bidirectional chat |

### 1.11 WebSocket Protocol

Connection lifecycle:
1. Client connects to `/ws/chat`
2. Client sends config JSON: `{provider, model, api_key?, session_id?}`
3. Server creates Agent with async approval callback (sends approval requests via WS)
4. Server sends `{type: "ready", session_id, title}`
5. Bidirectional message loop:
   - Client -> Server: `{type: "message", content, session_id?}`
   - Server -> Client: `{type: "session_start", session_id}` (when new response starts)
   - Server -> Client: `{type: "content", content}` (streaming chunks)
   - Server -> Client: `{type: "done", content, timestamp, has_tools, tool_calls, session_id, title}`
   - Server -> Client: `{type: "error", content}`
   - Client -> Server: `{type: "stop"}` (mid-generation cancel)
   - Client -> Server: `{type: "edit", index, content, session_id, message_id?}` (edit and regenerate)
   - Server -> Client: `{type: "approval_request", request_id, tool_name, arguments}`
   - Client -> Server: `{type: "approval_response", request_id, approved}`
   - Server -> Client: `{type: "title_updated", session_id, title}` (background title gen)
   - Server -> Client: `{type: "ping"}` (keepalive every 15s)
   - Client -> Server: `{type: "pong"}`
6. On disconnect: cancel any pending approvals (-> denied), cancel streaming task, cancel background tasks

**Edit flow**: Client sends edit with session_id. Server loads that session, finds the target message (by message_id preferred, then by index skipping system/tool messages), truncates history at that point, deserializes messages into agent memory, and re-runs the agent with the new prompt.

**SSE Streaming** (GET /stream): Queue-based streaming. Chunks are serialized as SSE data events.

### 1.12 Workflow Engine

State-machine-style workflow graph:

- **Node**: `(name, async_func: state -> state)`
- **Edge**: `(source, condition: (state -> node_name) | static_target)`
- **ParallelEdge**: `(source, targets[], reducer: ([state] -> state), next_node)`

Operations:
- `add_node(name, func_or_subgraph)` — registers a node (sub-graphs are wrapped automatically)
- `set_entry_point(name)`
- `add_edge(source, target)` — deterministic 1-to-1 transition
- `add_conditional_edge(source, condition)` — dynamic routing by state
- `add_parallel_edge(source, targets, reducer, next_node)` — concurrent execution with merge
- `run_streaming(initial_state)` -> async generator yielding `(node_name, state)` for each step
- `compile_and_run(initial_state)` -> synchronous run to completion
- `to_mermaid()` -> generates Mermaid state diagram

Features:
- Hard cap on iterations
- Checkpoint support: saves state to session metadata after each node
- Interrupt/resume: a special exception pauses execution
- Sub-graph nesting: a node can be another WorkflowGraph
- BEGIN/END terminal sentinels

### 1.13 Memory System

Handles conversation history summarization/pruning.

`summarize_if_needed(agent_messages, llm, max_messages, keep_recent?)`:
1. If message count <= max_messages, return as-is
2. Smart-select: keep system messages + tool interactions + recent messages + evenly sampled remainder
3. Summarize dropped messages via LLM
4. Store summary in session metadata (not in message list — it is injected into system prompt on subsequent calls)
5. Save session with updated metadata

Smart-select priority: 1. System messages -> 2. Tool interactions -> 3. Recent (40% of budget) -> 4. Evenly sampled remainder

### 1.14 Semantic Router

Routes user queries to sub-agents without polluting the main conversation history.

Two-phase routing:
1. **Heuristic** (fast path): Keyword matching against categories (code, file, web, memory, cli). Multiple keyword hits or single exact match routes to that agent.
2. **LLM** (fallback): Uses structured extraction with a RouteSelection model (reasoning + selected_agent). Temperature=0 for determinism. Hallucination guard: unknown agent names fall back to "default".

### 1.15 Callback System

**Callback** (abstract):
```
on_run_start(run_id, prompt)
on_run_end(run_id, response)
on_run_error(run_id, error)
on_llm_start(run_id, messages)
on_llm_end(run_id, response)
on_tool_start(run_id, tool_name, tool_kwargs)
on_tool_end(run_id, tool_name, result)
on_tool_error(run_id, tool_name, error)
```

**CallbackManager**: Broadcasts events to all registered callbacks.

Use cases:
- CLI tracer for latency debugging
- OpenTelemetry span creation for each event

### 1.16 Supporting Systems

**Circuit Breaker**: State machine (CLOSED -> OPEN -> HALF_OPEN). Protects provider calls. Configurable thresholds.

**Rate Limiter**: SQLite-backed, persists across restarts. Per-IP windows with atomic transactions. Includes global unlock-failure counter with exponential backoff.

**Metrics**: Counter and Histogram types. Prometheus text format export. Tracks: tool executions (name, duration, success/failure), LLM calls, message counts, session events, active connections.

**Logging**: Structured JSON logging with correlation IDs.

**Config**: YAML-based with user home directory config merged on top of project config. Environment variable overrides. Schema validation of all config sections. Deep merge strategy.

---

## PART 2: FRONTEND ARCHITECTURE

### 2.1 Component Tree

```
<App>
  ├── (loading) -> "Loading..."
  ├── (needs_setup) -> <SetupScreen onComplete>
  ├── (locked) -> <UnlockScreen onUnlocked>
  └── (main) -> <ChatProvider>
                ├── <ApprovalDialog />
                └── <div.app>
                    ├── <Sidebar />
                    │   ├── Title
                    │   ├── Model Selector (dropdown)
                    │   ├── Provider Selector (dropdown)
                    │   ├── New Chat button
                    │   ├── Search input
                    │   └── Session list
                    │       └── items: title + rename/delete buttons
                    └── <div.main-content>
                        ├── <Header>
                        │   ├── Model badge
                        │   ├── Connection status dot
                        │   ├── Lock button
                        │   ├── Theme toggle (dark/light)
                        │   ├── Debug panel toggle
                        │   └── <ChangePasswordDialog /> (conditional)
                        ├── <MessageList>
                        │   ├── Empty state
                        │   └── messages.map:
                        │       ├── User message (bubble)
                        │       └── Assistant message (full width)
                        │           ├── Thinking block (collapsible)
                        │           ├── Content (Markdown rendered)
                        │           ├── Tool calls (collapsible)
                        │           ├── Error display
                        │           ├── Typing indicator
                        │           └── Edit/Copy buttons
                        └── <ChatInput>
                            ├── Auto-resizing textarea
                            └── Send/Stop button
```

### 2.2 State Management

A single context provides all global state to every component:

**Data state:**
- `sessions[]`: `{id, title, created_at}`
- `activeSessionId: str?`
- `currentModel`, `currentProvider`: strings
- `models[]`, `providers[]`: available options
- `messages[]`: `{role, content, id?, timestamp?, thinking?, has_tools?, tool_calls?, error?}`
- `pendingApproval`: `{type, request_id, tool_name, arguments}?`

**Connection state:**
- `connectionStatus`: connected | connecting | disconnected | reconnecting
- `isConnected`, `isStreaming`: booleans
- `currentThinking`: string (streaming think content)

**Actions:**
- `sendMessage(content)` — adds user message to UI, sends over WebSocket
- `stopGeneration()` — sends stop over WebSocket
- `editMessage(index, newText, msgId?)` — updates locally, sends edit over WebSocket
- `retryMessage(index)` — re-sends the same user message
- `createSession()` — API call, clears messages
- `selectSession(id)` — API call, loads messages
- `deleteSession(id)`, `renameSession(id, title)` — API calls
- `selectModel(model)`, `selectProvider(provider)` — saves preference, reconnects WebSocket
- `reconnect()` — closes and reopens WebSocket
- `resolveApproval(requestId, approved)` — sends approval response over WebSocket

### 2.3 WebSocket Lifecycle

1. **Connect**: Called when model/provider changes. Creates new WebSocket to `/ws/chat`.
2. **onopen**: Sends config `{provider, model}`.
3. **onmessage**: Switch on event type:
   - `ready`: Connection established. Flush queued messages. Set active session.
   - `session_start`: Refresh session list.
   - `approval_request`: Stop streaming. Show approval dialog.
   - `title_updated`: Update session title in sidebar.
   - `content`: Streaming chunk — append to current assistant message.
   - `done`: Finalize message with content, tool_calls. Refresh sessions.
   - `error`: Display error on last user message.
4. **onclose**: Auto-reconnect with exponential jitter (500ms -> 30s max). Skip if close code 1000.
5. **Visibility change**: Reconnect if tab refocused and WebSocket not open.

**Message queue**: Messages sent before WebSocket is `ready` are queued and flushed on the `ready` event.

**Stale event protection**: Events with a session_id not matching the active session are discarded.

### 2.4 Communication Protocol Summary

| Channel | Path | Usage |
|---|---|---|
| **REST** | /api/status | Check lock/setup state on startup |
| **REST** | /api/unlock | Submit password, receive token |
| **REST** | /api/setup | Create initial password |
| **REST** | /api/logout | Invalidate token |
| **REST** | /api/change-password | Change database password |
| **REST** | /api/models?provider= | List available models |
| **REST** | /api/config | Get/set provider + model |
| **REST** | /api/preferences | Get/set user preferences |
| **REST** | /api/sessions | CRUD session list |
| **REST** | /api/sessions/{id} | Load/delete session |
| **REST** | /api/sessions/{id}/export | Export session JSON |
| **REST** | /api/sessions/import | Import session JSON |
| **REST** | /api/health | Health check |
| **REST** | /api/review | Review recommendations |
| **REST** | /api/chat | Sync chat (non-streaming) |
| **SSE** | /api/stream | Streaming chat |
| **WebSocket** | /ws/chat | Bidirectional chat + streaming + approvals + edits |

**Authentication**: Custom header `X-Unlock-Token` obtained from /api/unlock or /api/setup. Optional Bearer token auth via environment variable.

### 2.5 Frontend Features

- **Dark/light theme**: CSS variables, persisted to localStorage, restored before React hydrates
- **Markdown rendering**: Standard library + GFM plugin for tables, strikethrough
- **Thinking blocks**: `<think>` tags parsed and rendered as collapsible details elements
- **Tool calls**: Collapsible details showing tool name, JSON arguments, result, error
- **Message editing**: Inline textarea replaces message content; shortcuts to regenerate or cancel
- **Message copying**: Clipboard API with fallback
- **Auto-scroll**: Scrolls to bottom on new content unless user scrolled up
- **Typing indicator**: Three bouncing dots during streaming
- **Session search**: Client-side filter on session titles
- **Session rename**: Inline input on click, Enter to save, Escape to cancel
- **Delete confirmation**: Dialog with Escape to dismiss
- **Responsive layout**: Sidebar becomes slide-in drawer on mobile
- **Debug panel**: JSON dump of state, session debug export, change password
- **Approval dialog**: Modal overlay for dangerous tool operations, with danger-level warnings
- **Connection status**: Green/red indicator in header with connection state text

---

## PART 3: DATA FLOWS

### 3.1 Startup Flow

```
User starts backend
  -> bootstrap:
      -> load_config() (YAML + env overrides)
      -> validate_config()
      -> Password: first_run -> create new password
                  else      -> prompt for password
      -> create_agent(config, fernet)
          -> get_provider(provider, model, api_key, base_url)
          -> Agent(config, provider, fernet)
              -> SessionManager(session_dir, fernet)
                  -> recover_salt_transition() (crash recovery)
                  -> SQLite engine + migrations
              -> MemoryManager(session_manager)
      -> register_sub_agents() from config
      -> init OpenTelemetry if enabled
  -> Web: start HTTP server
  -> Frontend: GET /api/status
      -> {locked: true, needs_setup: false}  -> Show UnlockScreen
      -> {locked: false, needs_setup: true}  -> Show SetupScreen
      -> {locked: false, needs_setup: false} -> Show main UI
```

### 3.2 Chat Message Flow

```
User types message
  -> ChatInput: sendMessage(content)
    -> ChatProvider: add {role: "user", content} to messages[]
    -> ws.send({type: "message", content, session_id?})

Server receives
  -> Config validated
  -> Agent created (if first message)
  -> session_start event sent
  -> active_agent.run_streaming(prompt, on_chunk)
      -> Agent._run_loop_streaming()
        -> prepare_messages() format for LLM
        -> call LLM (streaming if available)
        -> For each chunk: on_chunk(chunk) -> websocket.send_json({type: "content", content})
        -> If tool_calls returned:
            -> execute_tool_calls() sequentially or in parallel
            -> If tool returns approval needed:
                -> ws.send_json({type: "approval_request", request_id, tool_name, args})
                -> wait for ws message {type: "approval_response", request_id, approved}
            -> Send tool results back to LLM for next iteration
        -> Final response: send {type: "done", content, tool_calls, ...}

Client receives "content" events
  -> ChatProvider: set isStreaming=true, update last message content

Client receives "done" event
  -> ChatProvider: set isStreaming=false, finalize message
  -> Refresh session list
  -> Display tool calls and thinking blocks

Background: generate_title()
  -> LLM call to summarize first user message
  -> ws.send_json({type: "title_updated", session_id, title})
```

### 3.3 Approval Flow

```
Server: tool execution requires approval
  -> ws.send({type: "approval_request", request_id, tool_name, arguments})
  -> Create a pending future, store in pending_approvals dict

Client: receive approval_request
  -> ChatProvider: set isStreaming=false, set pendingApproval
  -> ApprovalDialog renders
  -> User clicks Approve/Deny
  -> ws.send({type: "approval_response", request_id, approved})

Server: receive approval_response
  -> Set future result (true/false)
  -> Approval callback returns bool
  -> If approved: execute tool
  -> If denied: return "Operation denied by user"
```

### 3.4 Edit Message Flow

```
User clicks edit on a user message
  -> Inline textarea appears
  -> User modifies text, presses submit
  -> ws.send({type: "edit", index, content, session_id, message_id?})

Server receives edit
  -> Load session from DB
  -> Find target message (by message_id or by index skip system/tool)
  -> Validate it is a user message
  -> Truncate history at that message's position
  -> Deserialize truncated messages into agent memory
  -> Re-run agent with new prompt
  -> Stream response as normal
```

### 3.5 Session Encryption Flow

```
On server start (bootstrap or web unlock):
  -> Read salt_path and db_path
  -> if first_run:
      -> Create salt atomically (O_EXCL)
      -> derive_key(password, salt) -> Fernet key
      -> set global Fernet instance
  -> else:
      -> Read password from env or prompt
      -> Read salt from file
      -> derive_key(password, salt) -> Fernet key
      -> Scrub env var, zero password string
      -> set global Fernet instance
      -> SessionManager creates SQLite engine
          -> Encrypted type transparently encrypts/decrypts all columns
          -> Writes use process lock for serialization

On password change:
  -> Verify current password matches in-memory key
  -> Generate new salt
  -> Write-ahead new salt (temp-file + fsync + atomic rename)
  -> Acquire write lock
  -> Read all rows
  -> Decrypt each with old Fernet
  -> Re-encrypt each with new Fernet
  -> Update rows in single transaction
  -> Set marker in same transaction
  -> Commit
  -> Rename new salt -> live salt
  -> Reset marker
  -> Update in-memory Fernet
  -> Release lock

On crash during change password:
  -> Startup: crash recovery
  -> Read marker
  -> If marker=1 and temp salt exists: promote temp salt, reset marker
  -> If marker=0 and temp salt exists: delete temp salt (commit never completed)
  -> If marker=1 and no temp salt: reset marker (commit completed, rename was lost)
```

---

## PART 4: SUBSYSTEM SUMMARY

### Authentication & Access Control
- **Bearer token** (optional, configurable): gates all API and WebSocket
- **Unlock token** (custom header): issued on password submission, gates protected endpoints, single-process-wide (not per-user)
- **Rate limiting**: per-IP + global, SQLite-backed, configurable limits
- **No multi-user isolation**: design assumption is single trusted user per process

### Persistence
- **Sessions**: SQLite with ORM, encrypted at rest via Fernet
- **Preferences**: JSON file in user data directory
- **Salt**: binary file next to session database
- **Config**: YAML files (project + user overrides)
- **Rate limit data**: SQLite
- **Memory (notes)**: markdown files in notes directory
- **Memory (facts)**: SQLite
- **Vector store**: external vector database (optional, for RAG)
- **Audit log**: plaintext file

### Execution Safety
- Workspace confinement (path traversal prevention)
- Command allowlist/blocklist with glob patterns
- 25 dangerous command regex patterns
- Network domain allowlist
- Blocked file extensions and paths
- File size limits (10MB read, 5MB write)
- User approval callback for dangerous tools
- Approval via sync stdin or async WebSocket
- Audit logging of all approvals/denials
- ChangeTracker for undo/redo of file operations (with large-file spill-to-disk)
- Output truncation limits

### Observability
- Correlation IDs on every request
- Structured JSON logging
- Prometheus metrics (tool execution, LLM calls, sessions, connections)
- OpenTelemetry tracing (via callback system)
- CLI tracer for latency debugging

### Resilience
- Circuit breaker for LLM provider calls (CLOSED/OPEN/HALF_OPEN, configurable thresholds)
- Rate limiter (SQLite-backed, per-IP + global, atomic transactions)
- Retry logic (3 attempts, exponential backoff)
- Background summarization with stale-detection via generation counter
- Session save guards against summarization-triggered data loss
- Change-password crash recovery (write-ahead salt + marker)
- WebSocket reconnection with exponential jitter (500ms -> 30s)
- Stale WebSocket event protection

### Extensibility Points
- **Tools**: Implement base class (name, description, parameters_model, execute), register in tool registry
- **Providers**: Implement provider interface (chat, chat_streaming, extract_structured), add to provider factory
- **Search providers**: Implement search interface (search, get_limits), register in search_providers
- **Sub-agents**: Register with name, description, model, tools, system_prompt
- **Callbacks**: Implement callback interface, add to agent
- **Workflows**: Build graphs with nodes, edges, conditional edges, parallel edges
- **Config**: New sections in YAML -> new schema models -> env var overrides
