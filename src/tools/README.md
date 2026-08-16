# src/tools — tool registry and built-in tools

What this owns: the tool registry (`registry.c`), the `Tool` vtable
(`tool.h`), and every built-in tool — bash, git, python, file
read/write/replace, grep, web fetch/search, rest_api, deep_search,
semantic_search, notes, memory, sqlite query/schema, delegate,
ask_user, humanizer, ingest_document, browser. The browser tool wraps
the reusable CDP automation subsystem in `../browser` (spawns a
visible, real browser window the user can watch).

Why it exists: the agent exposes capabilities to the LLM through one
uniform execute/destroy contract; the registry resolves tool names,
enforces the enabled-set, and hosts the shared singletons (search
provider, OAuth handle, delegate config).

Ownership: `Tool` objects are registry-owned once registered and freed
by `registry_destroy()`. Per-tool contracts live in kernel-doc in each
tool's `.c` (the documented exception to one-header-per-module).
