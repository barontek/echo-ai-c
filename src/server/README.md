# src/server — HTTP/WS server and routes

What this owns: the libuv HTTP core (`server.c` — accept, streaming
parse with size limits, static files, response helpers), the RFC 6455
websocket layer (`websocket.c`), and the route handlers
(`routes/` — chat, ws chat, sessions, auth, general, openai auth).

Why it exists: one event-loop server serves both the REST API and the
WebSocket chat transport; all handlers share the loop-thread model
documented in the `routes/` headers.

Key contracts: response helpers transfer the client to the write
completion callback (callers must not touch the client afterwards);
websocket frames are serialized per connection by the loop.
