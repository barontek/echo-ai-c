# src/utils — shared utilities

What this owns: nine single-purpose modules — logging, metrics, circuit
breaker, rate limiter, string helpers, JSON helpers, HTML extraction,
the HTTP client, and the callback manager.

Why it exists: these are the only cross-cutting helpers; each module is
one header + one `.c` with a single responsibility (no catch-all
`utils.c`). The rate limiter and metrics carry explicit caller-
serialization contracts in their headers because they hold global
state.
