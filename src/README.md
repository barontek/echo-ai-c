# src — C core

What this owns: the entire server-side runtime — agent loop, LLM providers,
tool registry, HTTP/WS server, session storage, safety policy, and shared
utilities.

Why it exists: a single-binary C rewrite of the Echo AI agentic system with
no runtime beyond libuv/curl/sqlite/openssl/cjson. Each subdirectory is one
subsystem with its own header contract and README; see AGENTS.md for the
build-flag, memory-ownership, and documentation rules that govern this tree.

Module index:

- `agent/` — the conversation loop: LLM calls, tool execution, context
  windowing, session persistence, title generation.
- `llm/` — provider vtable (ollama/openai/openai-compatible/opencode-zen)
  plus the OpenAI OAuth client.
- `tools/` — the tool registry and every built-in tool.
- `server/` — libuv HTTP core, websocket layer, and the route handlers.
- `session/` — SQLite session store, Fernet-style field encryption,
  branch (fork/switch) support.
- `safety/` — path validation, approval gates, safety config.
- `config/` — the `.conf` parser.
- `change_tracker/` — file undo/redo snapshots.
- `utils/` — logging, metrics, circuit breaker, rate limiter, strings,
  JSON, html extraction, HTTP client, callbacks.
