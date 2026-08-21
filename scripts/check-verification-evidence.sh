#!/usr/bin/env bash
# check-verification-evidence.sh - rule A gate (AGENTS.md "Verification
# discipline"): fail→pass evidence cited as proof of a fix must come from
# an actual command invocation in that session — the artifact must show the
# raw command AND its output, not a paraphrase or a claim.
#
# Blocking from day one (structural check, no false-positive risk worth a
# graduation period). Scope: docs/verification/*.md touched by the diff
# (added or modified); untouched files are grandfathered.
#
# A file passes if EITHER:
#   (a) it contains >= FENCED_BLOCKS_MIN distinct fenced code blocks, or
#   (b) it contains a fenced block whose first content line is a command
#       with a `$ ` prefix, followed by >= OUTPUT_LINES_MIN non-blank lines
#       of raw output.
# A single fenced block holding only a command with no output, or prose
# claims, does NOT pass — that is the failure mode this gate exists to stop.
#
# Usage: scripts/check-verification-evidence.sh [base_ref]
#   base_ref: git ref to diff against (default: origin/master, else HEAD~1).

set -euo pipefail
cd "$(dirname "$0")/.."

FENCED_BLOCKS_MIN="${FENCED_BLOCKS_MIN:-2}"
OUTPUT_LINES_MIN="${OUTPUT_LINES_MIN:-3}"

base="${1:-}"
if [ -z "$base" ]; then
    if git rev-parse --verify origin/master >/dev/null 2>&1; then
        base=origin/master
    else
        base=HEAD~1
    fi
fi

mapfile -t FILES < <(git diff --name-only --diff-filter=ACMR "$base"...HEAD -- 'docs/verification/*.md' 2>/dev/null || true)

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "verification evidence: no docs/verification/*.md touched vs $base — OK"
    exit 0
fi

FAIL=0
for f in "${FILES[@]}"; do
    [ -f "$f" ] || continue
    result=$(awk -v minblocks="$FENCED_BLOCKS_MIN" -v minout="$OUTPUT_LINES_MIN" '
        function is_cmd(l) { return l ~ /^\$ / }
        BEGIN { inblock=0; blocks=0; anyok=0; first=0; cmd_ok=0; out=0 }
        /^```+/ {
            if (!inblock) { inblock=1; blocks++; first=0; cmd_ok=0; out=0 }
            else { if (cmd_ok && out >= minout) anyok=1; inblock=0 }
            next
        }
        inblock && !first { first=1; if (is_cmd($0)) cmd_ok=1; next }
        inblock && first && cmd_ok && $0 !~ /^[ \t]*$/ { out++ }
        END {
            if (cmd_ok && out >= minout) anyok=1
            if (blocks >= minblocks || anyok) print "PASS"; else print "FAIL"
        }
    ' "$f")
    if [ "$result" = "FAIL" ]; then
        FAIL=1
        echo "FAIL $f: no raw command+output evidence (need >= $FENCED_BLOCKS_MIN fenced blocks, or a \"\$ <command>\" block with >= $OUTPUT_LINES_MIN output lines)"
    else
        echo "ok   $f"
    fi
done

if [ "$FAIL" -ne 0 ]; then
    echo "verification evidence gate: FAILED — show the raw command and its output, not a claim (AGENTS.md 'Verification discipline')."
    exit 1
fi
echo "verification evidence gate: OK"
