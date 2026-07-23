# Echo AI — C Implementation Plan

## Overview

A lean, self-contained C rewrite of the Echo AI agentic system. Single binary, minimal dependencies, vanilla JS frontend.

## Directory Structure

```
echo-ai/
├── CMakeLists.txt
├── src/
│   ├── main.c                       # Entry point: --cli / --chat / --web
│   ├── agent/
│   │   ├── agent.h / .c             # Core agent loop: run(), run_streaming()
│   │   ├── message.h / .c           # Message structs, LLM formatting
│   │   ├── context.c                # apply_context_window, smart_select, trim
│   │   └── summarizer.c             # Background LLM summarization
│   ├── llm/
│   │   ├── provider.h               # Abstract LLM provider interface
│   │   ├── ollama.c                 # OllamaProvider (HTTP to localhost:11434)
│   │   └── factory.c                # get_provider() factory function
│   ├── tools/
│   │   ├── tool.h                   # Base Tool struct + execute() interface
│   │   ├── registry.c               # Tool registry + JSON schema generation
│   │   ├── bash.c                   # Shell execution with safety validation
│   │   ├── read_file.c              # Read file with path/size safety
│   │   ├── write_file.c             # Write file with change tracking
│   │   ├── list_dir.c               # Directory listing
│   │   ├── glob_tool.c              # File pattern matching
│   │   ├── grep_tool.c              # Content search
│   │   ├── web_fetch.c              # HTTP GET via libcurl
│   │   └── web_search.c             # Web search via configurable provider
│   ├── server/
│   │   ├── server.h / .c            # libuv TCP listener + HTTP parser
│   │   ├── websocket.c              # RFC 6455 handshake + framing
│   │   ├── routes.c                 # All REST API endpoint handlers
│   │   ├── middleware.c             # Auth, rate limiting, CORS, logging
│   │   └── static.c                 # Serve frontend static files
│   ├── session/
│   │   ├── session.h / .c           # Session domain object
│   │   ├── session_manager.c        # CRUD, save/load, checkpoint, export
│   │   ├── encryption.c             # Fernet (AES-128-CBC + HMAC-SHA256) + Scrypt KDF
│   │   └── migration.c              # Schema migration + crash recovery
│   ├── safety/
│   │   ├── safety.h                 # SafetyConfig + SecurityValidator types
│   │   ├── validator.c              # Path traversal, blocked patterns, commands
│   │   ├── approval.c               # Sync/async approval callback system
│   │   └── config.c                 # SafetyConfig loading from parsed config
│   ├── config/
│   │   ├── config.h / .c            # .conf parser + config structs
│   │   └── schema.h                 # All config struct definitions
│   ├── utils/
│   │   ├── json.c                   # cJSON wrappers for LLM/tool formatting
│   │   ├── http_client.c            # libcurl wrapper (GET, POST, streaming)
│   │   ├── logging.c                # Structured JSON logging w/ correlation IDs
│   │   ├── metrics.c                # Prometheus counter/histogram support
│   │   ├── circuit_breaker.c        # CLOSED/OPEN/HALF_OPEN state machine
│   │   ├── rate_limiter.c           # SQLite-backed per-IP + global rate limiter
│   │   └── string_utils.c           # trim, split, sanitize, dedent
│   └── change_tracker/
│       ├── change_tracker.h / .c    # File undo/redo with large-file spill
├── web/
│   ├── index.html                   # SPA shell
│   ├── css/style.css                # Dark/light theme, layout, components
│   └── js/
│       ├── app.js                   # State management, routing, boot
│       ├── chat.js                  # Message list, rendering, streaming
│       ├── websocket.js             # WS connect/reconnect, message dispatcher
│       ├── api.js                   # REST API client (status, unlock, sessions)
│       └── components.js            # Sidebar, dialogs, input, approvals
├── tests/
│   ├── CMakeLists.txt
│   ├── test_agent.c
│   ├── test_encryption.c
│   ├── test_safety.c
│   └── test_*.c
├── config.conf.example
└── README.md
```

## Dependencies

| Library    | Use                                          |
|------------|----------------------------------------------|
| libuv      | Event loop + TCP for HTTP/WS server           |
| libcurl    | HTTP client for Ollama, web_fetch, web_search |
| sqlite3    | Session DB, rate limiter                      |
| openssl    | AES-128-CBC, HMAC-SHA256, Scrypt, SHA1        |
| cJSON      | JSON parse/serialize                          |
| Check      | Unit testing (dev only)                       |

**5 runtime deps.** All widely available on every Linux distro.

## Config (.conf format)

Strict, boring key-value parser. Not real YAML.

```
# comment
key = value

[section]
nested_key = value
```

- `#` line comments only
- `[section]` headers for nesting
- `key = value` — no quotes needed, trimmed whitespace
- No type coercion (everything is string; consumers parse as needed)
- Error on tabs, flow syntax, multi-document markers

## WebSocket (RFC 6455)

Pure server-side implementation on libuv TCP, no external dep:

### Upgrade handshake
1. Read HTTP upgrade request, extract `Sec-WebSocket-Key`
2. Compute `SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")`
3. Respond with `101 Switching Protocols` + `Sec-WebSocket-Accept: <base64(sha1)>`

### Framing
- Read: 2-byte header → mask + opcode + length → extended length if 126/127 → masking key (4 bytes) → payload
- Write: same but server never masks (MSB=0)
- Opcodes: 0x1 (text), 0x8 (close), 0x9 (ping), 0xA (pong)
- Close frames: read status code, echo back

### Keepalive
- Server sends ping every 15s (matches arch doc)
- Client must respond with pong
- No response → close + cleanup

## Frontend WS → UI Wireup

Message dispatcher pattern in `websocket.js`:

```js
const handlers = {
  ready:            (d) => { setSession(d.session_id); flushQueue(); },
  content:          (d) => { appendChunk(d.content); },
  done:             (d) => { finalizeMessage(d); },
  session_start:    (d) => { refreshSessions(); },
  approval_request: (d) => { showApprovalDialog(d); },
  title_updated:    (d) => { updateSidebarTitle(d); },
  error:            (d) => { showError(d); },
};

// onmessage:
//   const data = JSON.parse(event.data);
//   (handlers[data.type] ?? (() => console.warn('unknown type', data.type)))(data);
```

Each handler is a focused function. Streaming chunks innerHTML-append to the last assistant message bubble. No virtual DOM needed.

## Frontend State

Single state object in `app.js`:

```js
const state = {
  status: 'loading',           // loading | locked | setup | ready
  sessionId: null,
  sessions: [],                // {id, title, created_at}
  messages: [],                // {role, content, id, timestamp, thinking?, tool_calls?}
  isStreaming: false,
  currentThinking: '',
  pendingApproval: null,       // {request_id, tool_name, arguments}
  connectionStatus: 'disconnected',
  currentModel: '',
  currentProvider: '',
  models: [],
  providers: [],
  theme: 'dark',
};
```

## Build Phases

### Phase 1 — Foundation
- CMake project skeleton
- `src/main.c` with arg parsing (--cli, --chat, --web)
- Config parser (.conf)
- Logging (structured JSON to stderr)
- String utilities

### Phase 2 — CLI Agent
- Message structs + JSON formatting
- LLM provider interface
- Ollama provider (libcurl POST to `/api/chat`, stream SSE)
- Core agent loop: non-streaming first, then streaming
- Context window: trim-messages, smart-select, think-tag parser

### Phase 3 — Tool System
- Tool base: `name`, `description`, `parameters_schema`, `execute()`
- Tool registry with auto-generated JSON schema for LLM
- Bash tool with command safety (allowlist, dangerous patterns, sub-command splitting)
- Read/Write file tools with path traversal protection, blocked extensions, size limits
- List directory tool
- Glob tool (fnmatch-style pattern matching)
- Grep tool (line-by-line content search)
- Web fetch tool (libcurl GET, domain allowlist, size limit)
- Web search tool (pluggable search providers)

### Phase 4 — Session Management
- SQLite schema: `agent_sessions` table with encrypted columns
- Scrypt KDF (N=2^18, r=8, p=1 → 32-byte key → base64 Fernet key)
- Fernet encrypt/decrypt: AES-128-CBC + HMAC-SHA256
- Session manager: create, load, save (write lock), delete, list, export, import
- Password resolution: env var → interactive prompt → exit
- First-run detection + atomic salt creation
- Change-password crash recovery (write-ahead salt + marker)
- Wire persistence into agent loop (save on each step)

### Phase 5 — Web Server
- libuv TCP listener on port 8080
- HTTP request parser (method, path, headers, body)
- REST API: /api/status, /api/setup, /api/unlock, /api/logout, /api/config, /api/sessions/*, /api/chat, /api/health
- WebSocket: /ws/chat with full lifecycle (config → ready → message → streaming → done → edit → approval)
- SSE: GET /stream for non-WS streaming
- Middleware: correlation ID, bearer token auth, rate limiting, unlock token, request logging
- Approval flow: WS approval_request → client → WS approval_response
- Static file serving for web/ directory

### Phase 6 — Frontend
- index.html shell with dark/light CSS variables
- CSS: layout (sidebar + main), message bubbles, thinking blocks, tool call details, approval dialog, responsive sidebar
- JS: app.js (state + routing), chat.js (message rendering + streaming), websocket.js (connect/reconnect/dispatcher), api.js (REST helpers), components.js (sidebar, dialogs, input)
- Screens: loading → setup → unlock → main chat
- Features: message list, streaming render, think/tool collapsibles, approval dialog, session sidebar (list/search/rename/delete), model selector, dark/light toggle, connection indicator

### Phase 7 — Polish
- Circuit breaker (CLOSED/OPEN/HALF_OPEN for Ollama calls)
- Metrics (Prometheus-format /api/metrics)
- Change tracker (undo/redo for file writes)
- Unit tests with Check framework

### Phase 8 — Documentation
- `README.md`: project overview, build steps, CLI flags, dependency table, config reference
- `config.conf.example`: fully-documented reference covering every config key/section
- `echo-ai.1` (man page): CLI synopsis, flags, config file format, environment variables
- Doc comments on public functions/structs in `.h` headers (brief, one-liner only where useful)

## Verification Checklist

- [x] **P1**: `./echo-ai --help` prints usage; `--cli`, `--chat`, `--web` dispatch correctly
- [x] **P2**: CLI REPL starts, creates agent, makes Ollama request, streams/returns response
- [x] **P3**: Each essential tool works via agent (bash, read, write, ls, glob, grep, web_fetch, web_search)
- [x] **P4**: Sessions persist across restarts, encrypt/decrypt correctly; password change + crash recovery works
- [x] **P5**: HTTP server starts, serves REST endpoints, WebSocket streams chat, approval flow works
- [x] **P6**: Full chat loop in browser: unlock → create session → send message → stream response → tool calls → approval → done
- [x] **P7**: Tests pass, metrics export, undo/redo works, circuit breaker protects against LLM failures
- [x] **P8**: README, man page, config.conf.example, header doc comments all written
