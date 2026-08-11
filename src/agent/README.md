# src/agent — the agent loop

What this owns: one conversation's lifecycle — `agent_run_streaming_new`,
tool-call execution, context windowing with summarization, title
generation, and session persistence. The `Message` model lives here too
(`message.c`), shared with session storage.

Why it exists: separates the conversation logic from transport
(`src/server`) so the same agent can run under CLI, HTTP, or WebSocket
frontends without duplicating the loop.

Key contracts (see `agent.h`): the agent is not thread-safe; one shared
Agent instance is mutated by the server's loop thread. Ownership rules
for returned `LLMResponse` and appended `Message` objects are documented
on each function.
