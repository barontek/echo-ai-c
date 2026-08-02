#!/usr/bin/env bash
# Start echo-ai behind a Caddy TLS reverse proxy.
# Dev-only: everything lives in the repo / nix develop — no system changes.
# Usage: nix develop -c ./scripts/serve-tls.sh
#        CONFIG=my.conf nix develop -c ./scripts/serve-tls.sh
set -euo pipefail

CONFIG="${CONFIG:-config.conf}"
CADDYFILE="${CADDYFILE:-deploy/Caddyfile}"
BUILD_DIR="${BUILD_DIR:-build}"

"$BUILD_DIR/echo-ai" --web --config "$CONFIG" &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT INT TERM

echo "echo-ai running (PID $SERVER_PID); starting caddy TLS proxy..."
caddy run --config "$CADDYFILE" --adapter caddyfile
