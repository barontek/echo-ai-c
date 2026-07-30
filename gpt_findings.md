# GPT Code Review Findings

Validation status: all findings below were confirmed against the codebase before remediation work began.

## Critical

1. **Unauthenticated WebSocket access**
   - `/ws/chat` bypasses unlock middleware.
   - An unauthenticated client can create an agent, load a known session, and submit tool approval responses.
   - References: `src/server/server.c:263`, `src/server/routes/routes_ws.c:282`, `src/server/routes/routes_ws.c:633`.

2. **Static-file path traversal**
   - Raw request paths are appended to `frontend/dist` without normalization or containment checks.
   - A request containing `../` can disclose files outside the frontend directory.
   - Reference: `src/server/server.c:186`.

3. **Context-window use-after-free**
   - Context selection shallow-copies owned `Message` fields.
   - The original messages are then cleared, leaving the selected messages with dangling pointers.
   - References: `src/agent/context.c:69`, `src/agent/context.c:181`, `src/agent/agent.c:340`.

## High

4. **Unsafe and fragmentation-broken HTTP parser**
   - The parser calls unbounded C string functions on non-NUL-terminated network buffers.
   - Partial headers are discarded instead of accumulated.
   - References: `src/server/server.c:323`, `src/server/server.c:420`.

5. **Asynchronous WebSocket writes use stack buffers**
   - Ping, pong, and close frames pass stack storage to `uv_write`, whose buffers must remain valid until completion.
   - References: `src/server/websocket.c:31`, `src/server/websocket.c:136`, `src/server/websocket.c:295`.

6. **Workspace confinement is ineffective**
   - Absolute paths are accepted unchanged and symlink containment is not checked.
   - Several file tools can therefore operate outside the configured workspace.
   - References: `src/safety/safety.c:127`, `src/safety/safety.c:309`.

7. **Disabled tools remain executable**
   - Tool enablement controls schema exposure but is not checked at execution time.
   - References: `src/tools/registry.c:155`, `src/agent/agent.c:122`.

8. **Global `ask_user` callback retains per-WebSocket state**
   - Connections overwrite a process-global callback and userdata pointer.
   - Disconnecting can leave a dangling pointer, and concurrent clients can receive each other's questions.
   - References: `src/tools/registry.c:56`, `src/server/routes/routes_ws.c:481`, `src/server/routes/routes_ws.c:631`.

9. **Notes path traversal**
   - User-controlled note names are interpolated into paths without basename validation.
   - Read, write, and delete can escape the notes directory.
   - Reference: `src/tools/notes.c:100`.

10. **Predictable and non-exact unlock tokens**
    - Tokens are generated from time and `rand()`.
    - Header validation accepts the correct token followed by an arbitrary suffix.
    - References: `src/server/routes/routes_auth.c:79`, `src/server/middleware.c:21`.

## Medium

11. **Bash output deadlock and surviving descendants**
    - The parent waits for the child before draining the output pipe.
    - Large output blocks the child, and timeout kills only the direct shell process.
    - Reference: `src/tools/bash.c:19`.

12. **Delegate tool calls are moved before execution**
    - Tool-call fields are moved out of the response before the code checks whether calls remain.
    - Normal delegated tool calls are therefore skipped.
    - Reference: `src/tools/tool_delegate.c:162`.

13. **SSE chunks produce invalid JSON**
    - Raw model text is interpolated as a JSON value without quoting or escaping.
    - Reference: `src/server/routes/routes_chat.c:90`.

14. **Session-list growth can double-free after partial `realloc` success**
    - Four reallocations are attempted before ownership is committed.
    - A partial success followed by failure leaves stale pointers that are freed again.
    - Reference: `src/session/session_manager.c:779`.

15. **URL policy bypass, redirect escape, and unbounded buffering**
    - Allowed domains are matched as raw URL substrings.
    - Redirect destinations are not revalidated.
    - Response limits are applied after buffering, or not at all.
    - References: `src/safety/safety.c:261`, `src/tools/web_fetch.c:22`, `src/tools/rest_api.c:22`.

16. **LM Studio streaming returns an empty response**
    - Stream chunks are forwarded but never accumulated into the returned `LLMResponse`.
    - Streamed tool-call deltas are also discarded.
    - References: `src/llm/lmstudio.c:217`, `src/llm/lmstudio.c:305`.

## Verification Baseline

- Existing build completed successfully before remediation.
- Existing test suite passed 27/27 tests before remediation.
- `src/server/server.c` and `src/server/websocket.c` were not covered by the route test targets.
- Test targets did not consistently inherit the required warning and sanitizer flags.

## Remediation Results

All 16 findings held up under code-path review. The current worktree contains the following remediations:

1. **Fixed:** WebSocket upgrades require the current unlock token through `Sec-WebSocket-Protocol`. Connections are bound to an authentication generation, so logout and token rotation invalidate existing sockets.
2. **Fixed:** Static request paths reject dot segments, canonical containment is checked, and files are opened component-by-component beneath the static root with `openat` and `O_NOFOLLOW`.
3. **Fixed:** Context selection deep-copies complete messages and tool-call results before the original history is released.
4. **Fixed:** HTTP bytes are accumulated across reads, NUL-terminated internally, bounded, parsed with checked lengths, and dispatched only once. Invalid or unsupported body framing is rejected.
5. **Fixed:** WebSocket control and data frames use heap storage through asynchronous completion; synchronous submission failures clean up immediately.
6. **Fixed:** File paths are canonicalized against the workspace. Absolute escapes, traversal, symlink escapes, recursive grep symlinks, and out-of-workspace glob results are rejected.
7. **Fixed:** Runtime registry lookup returns enabled tools only, making schema exposure and execution policy consistent.
8. **Fixed for the current single-threaded event loop:** `ask_user` callbacks are scoped around the originating agent's tool execution, and WebSocket contexts are retained until active runs unwind after disconnect. A future multi-threaded agent runner should replace the remaining scoped global bridge with an explicit execution context.
9. **Fixed:** Note names are restricted to basenames; reads and writes use `O_NOFOLLOW`, writes are size-limited, and all stream close paths are handled.
10. **Fixed:** Unlock tokens use 32 bytes from OpenSSL `RAND_bytes`, exact constant-time comparison, and transactional setup ownership. WebSocket protocol tokens also require an exact match.
11. **Fixed:** Bash output is drained concurrently with process monitoring. Commands run in a dedicated process group, retained output is bounded, and descendants receive TERM followed by KILL escalation.
12. **Fixed:** Delegate history deep-copies tool calls without clearing the executable response before dispatch.
13. **Fixed:** SSE event payloads are serialized with cJSON, preserving quotes, newlines, and event boundaries.
14. **Fixed:** Session-list arrays are reallocated and committed sequentially, with fault-injection coverage for each partial-growth failure.
15. **Fixed:** URLs are parsed by scheme and authority, allowlists compare host boundaries, redirects and proxies are disabled, response limits apply while downloading, and libcurl socket creation rejects local/private/special-use destinations after DNS resolution.
16. **Fixed:** LM Studio streaming accumulates response content and fragmented tool-call fields into the returned `LLMResponse` while forwarding chunks.

## Final Verification

- Fast-forwarded to `origin/master` at `601d856` before final integration; overlapping upstream fixes were retained.
- Debug build completed with `-Wall -Wextra -Wpedantic -Werror` and ASan/UBSan.
- CTest passed 34/34 tests under ASan/UBSan with macOS leak detection disabled as documented by the project.
- Frontend TypeScript and Vite production build completed successfully.
- Added direct regression coverage for HTTP fragmentation, static traversal, delayed WebSocket writes, exact authentication, context ownership, session realloc failures, notes traversal, recursive grep symlinks, process output/descendants, delegate calls, SSE escaping, and LM Studio streaming.
- Added libFuzzer targets for the HTTP parser and LM Studio SSE stream parser when the compiler supports `-fsanitize=fuzzer`.
