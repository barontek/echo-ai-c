# src/safety — path validation and approvals

What this owns: the `SafetyConfig` (workspace roots, blocked paths,
allowed/blocked extensions and commands, domain allowlists, approval
gates) and the checks every tool must pass before touching the file
system or the network.

Why it exists: tools are arbitrary user/LLM-controlled code paths; a
single enforcement point keeps the "no escaping the workspace, no
approval bypass" invariant testable in one place.
