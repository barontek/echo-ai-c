# src/llm — LLM providers

What this owns: the `LLMProvider` vtable (`provider.h`) and its four
implementations — ollama, openai, openai-compatible, and opencode-zen —
plus the OpenAI OAuth client (`openai_oauth.c`), which embeds a local
HTTP callback server for device authorization.

Why it exists: every provider speaks the same chat/chat_streaming/
extract_structured contract, so the agent loop never needs to know which
backend it is talking to. The vtable contract (ownership, concurrency,
failure modes) is documented in `provider.h` — the one header in this
tree without a matching `.c` (a shared contract, not an orphan module).
