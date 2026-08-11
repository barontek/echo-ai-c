#!/usr/bin/env bash
# check-file-lengths.sh - audit src/ and tests/ files against the AGENTS.md
# file-size bands: 800-line ceiling for src/tests, 600 for headers. Exits
# non-zero when any file exceeds its band so CI can gate (or report).
#
# Usage: scripts/check-file-lengths.sh [ceiling]   (default ceiling 800)
set -u

CEILING="${1:-800}"
HEADER_CEILING=600
REPO="$(cd "$(dirname "$0")/.." && pwd)"
FAIL=0

while IFS= read -r file; do
    [ -f "$file" ] || continue
    lines=$(wc -l < "$file")
    case "$file" in
        *.h) limit=$HEADER_CEILING ;;
        *)   limit=$CEILING ;;
    esac
    if [ "$lines" -gt "$limit" ]; then
        printf 'OVER %d lines: %s (%d)\n' "$limit" "${file#"$REPO"/}" "$lines"
        FAIL=1
    fi
done < <(find "$REPO/src" "$REPO/tests" -name '*.c' -o -name '*.h' | sort)

if [ "$FAIL" -eq 0 ]; then
    echo "file lengths OK (src/tests <= $CEILING, headers <= $HEADER_CEILING)"
fi
exit "$FAIL"
