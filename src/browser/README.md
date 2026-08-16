# src/browser — CDP browser automation subsystem

What this owns: the browser session lifecycle (`browser.c`) and the
minimal Chrome DevTools Protocol transport (`cdp.c`). The subsystem is
browser-agnostic: it drives any Chromium-family browser (Brave, Chrome,
Edge, Chromium, ...) over `--remote-debugging-pipe` and never hardcodes
one.

Why it exists: the agent needs a real, visible browser for pages that
plain libcurl cannot handle — JS challenges, SPAs, interactive sites.
`browser_fetch_text()` is the layer web_fetch can adopt as its
challenge fallback instead of shelling out to curl-impersonate.

Layout:

- `cdp.h`/`cdp.c` — transport only: NUL-framed JSON over a pipe pair
  (Chromium's ASCIIZ pipe protocol: browser reads fd 3, writes fd 4),
  background reader thread, id-matched responses with timeouts.
  Consumable by any future CDP user (browser.c is the only one today).
- `browser.h`/`browser.c` — binary discovery, spawn, session lifecycle,
  and page operations: `browser_navigate`, `browser_evaluate`,
  `browser_screenshot`, `browser_fetch_text`.

Configuration (all optional, nothing hardcoded):

- `ECHO_BROWSER_BIN` — explicit browser path or binary name (first
  priority).
- `$BROWSER` — xdg-style fallback.
- Otherwise: PATH lookup of brave/chrome/chromium/edge/opera/vivaldi,
  then macOS app bundles.
- `ECHO_BROWSER_HEADLESS=1` — run without a window (default: visible).
- `ECHO_BROWSER_USER_DATA_DIR` — persistent profile (default: temp
  dir, deleted on close).
- `ECHO_BROWSER_FLAGS` — extra args appended to the command line.

Security notes: a spawned browser cannot be socket-policy-checked the
way libcurl calls are; the browser's own networking is out of the
safety config's reach. Treat this as an explicit user-facing capability
with visible windows, not a sandbox.

Ownership: `BrowserSession` is caller-owned, freed by
`browser_close()` (kills the child, joins the CDP transport, removes
the temp profile). The CDP client is owned by the session except in
test-attached sessions (`browser_test_attach`). All API functions are
documented in `browser.h`; the browser layer is not thread-safe —
serialize calls per session.
