#!/usr/bin/env bash
# cppcheck.sh - cppcheck static-analysis gate (CI static-analysis job).
#
# Scope: src/*.c only, matching scripts/lint.sh — the gate covers the
# shipped code; tests are exercised directly by ctest under sanitizers.
# The CMake compile commands are filtered down to src/ entries first
# (needs python3), so include paths and defines come from the real build.
#
# Findings exit non-zero — the same status as a warning under -Werror per
# AGENTS.md "Static analysis". Per-line exceptions go through
# `// cppcheck-suppress <id>` plus a `// TODO(reason:)` comment; never a
# blanket pragma.
#
# Suppressed check-wide (documented idioms, not per-site variance):
#   varFuncNullUB  — the repo's log_error(msg, NULL) variadic-sentinel
#     idiom (263 hits at triage). NULL expands to ((void*)0) on glibc and
#     clang, which is exactly what CERT wants in a variadic slot; cppcheck
#     models platforms where NULL is a bare 0.
#   missingInclude / missingIncludeSystem — system/3rd-party headers the
#     analyzer cannot open; everything else is a finding.
# unmatchedSuppression stays ON so a stale `// cppcheck-suppress` comment
# surfaces instead of silently suppressing forever.
#
# Usage: scripts/cppcheck.sh [build_dir]   (default build_dir "build")

set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build}"

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    echo "error: $BUILD_DIR/compile_commands.json missing — configure first:" >&2
    echo "  cmake -B $BUILD_DIR -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
    exit 1
fi

FILTERED_DIR="$(mktemp -d)"
trap 'rm -rf "$FILTERED_DIR"' EXIT
python3 - "$BUILD_DIR/compile_commands.json" "$FILTERED_DIR/compile_commands.json" <<'PY'
import json, sys
with open(sys.argv[1]) as fh:
    db = json.load(fh)
src = [e for e in db if "/src/" in e["file"]]
with open(sys.argv[2], "w") as fh:
    json.dump(src, fh)
print(f"cppcheck: {len(src)} translation units under src/", file=sys.stderr)
PY

echo "cppcheck: $(cppcheck --version | sed 's/^Cppcheck //') over src/ via filtered compile_commands"
cppcheck --project="$FILTERED_DIR/compile_commands.json" \
    --enable=warning,performance,portability \
    --error-exitcode=1 \
    --inline-suppr \
    --std=c11 \
    --platform=unix64 \
    --suppress=varFuncNullUB \
    --suppress=missingInclude \
    --suppress=missingIncludeSystem \
    -j "$(nproc 2>/dev/null || echo 2)" \
    --template='{file}:{line}: {severity}: {message} [{id}]'
