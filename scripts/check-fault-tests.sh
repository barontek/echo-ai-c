#!/usr/bin/env bash
# check-fault-tests.sh - rule B merge gate (AGENTS.md "Verification
# discipline"): any new function with a non-trivial multi-allocation commit
# path must ship a fault-injection test in the same PR, or the PR is blocked
# — enforced as a CI check, not a reviewer-remembers-to-check convention.
#
# Report-only with graduation: the "new multi-allocation function" detector
# is a heuristic whose false-positive/negative rate must be measured against
# this repo before it may safely block. REPORT_SINCE / FLIP_DATE below are
# the tracked dates (no docs/ tracking — those files are not maintained).
# Once the current date passes FLIP_DATE this script prints a banner urging
# the flip to blocking (or a date update); MODE=blocking (or --blocking)
# turns findings into a non-zero exit.
#
# Detection heuristic: for every src/**/*.c changed between base and HEAD,
# a function present in HEAD but absent in base that contains >= MIN_ALLOCS
# allocation calls (malloc/calloc/realloc/reallocarray/strdup/str_dup/
# asprintf/str_asprintf/vasprintf) is a "multi-allocation commit site".
# Each is matched against a fault test by repo-wide search for
# tests/**/test_<basename>.c (the test tree is not a strict mirror —
# src/config/config.c lives in tests/utils/test_config.c), and THAT matched
# file's content must contain a fault-injection marker (alloc-fail hook,
# fail_at counter, or a test allocator #define) — existence alone is not
# enough.
#
# Usage: scripts/check-fault-tests.sh [base_ref] [--blocking]
#   base_ref: git ref to diff against (default: origin/master, else HEAD~1).
#   Requires python3 (the function-body parser).

set -euo pipefail
cd "$(dirname "$0")/.."

REPORT_SINCE="2026-08-18"
FLIP_DATE="2026-09-08"      # target date to flip MODE to blocking
MIN_ALLOCS="${MIN_ALLOCS:-2}"
MODE="${MODE:-report}"      # report|blocking — flip after graduation

base="${1:-}"
[ "$1" = "--blocking" ] && { MODE=blocking; base="${2:-}"; }
if [ -z "$base" ]; then
    if git rev-parse --verify origin/master >/dev/null 2>&1; then
        base=origin/master
    else
        base=HEAD~1
    fi
fi

today="$(date +%Y-%m-%d)"
if [[ "$today" > "$FLIP_DATE" ]]; then
    days=$(( ($(date -d "$today" +%s) - $(date -d "$FLIP_DATE" +%s)) / 86400 ))
    echo "NOTE: rule B (fault-injection merge gate) has been report-only for ${days} day(s) past its target flip date (${FLIP_DATE}). Flip to blocking (MODE=blocking) or update FLIP_DATE in scripts/check-fault-tests.sh." >&2
fi

mapfile -t SRC_FILES < <(git diff --name-only --diff-filter=ACMR "$base"...HEAD -- src/ 2>/dev/null | grep '\.c$' || true)

if [ "${#SRC_FILES[@]}" -eq 0 ]; then
    echo "fault-injection gate: no src/*.c changed vs $base — OK"
    exit 0
fi

# Fault-injection markers present in test files (alloc-fail hooks, fail_at
# counters, or a redefined test allocator per the AGENTS.md pattern).
MARKER_RE='set_[A-Za-z0-9_]*fail|fail_at|alloc_fail|realloc_fail|#define[[:space:]]+str_dup[[:space:]]+test_strdup'

FAILURES=0
FOUND=0
for f in "${SRC_FILES[@]}"; do
    head_tmp="$(mktemp)"; base_tmp="$(mktemp)"; py_tmp="$(mktemp)"
    git show "HEAD:${f}" > "$head_tmp" 2>/dev/null || : > "$head_tmp"
    git show "${base}:${f}" > "$base_tmp" 2>/dev/null || : > "$base_tmp"

    set +e
    MIN_ALLOCS="$MIN_ALLOCS" python3 - "$head_tmp" "$base_tmp" > "$py_tmp" <<'PY'
import os, re, sys

KEYWORDS = frozenset("if for while switch return sizeof do".split())
ALLOC_RE = re.compile(
    r"\b(malloc|calloc|reallocarray|realloc|strdup|str_dup|asprintf|str_asprintf|vasprintf)\s*\("
)

def strip_c(code):
    out = []
    i, n = 0, len(code)
    state = "code"
    while i < n:
        c = code[i]
        nxt = code[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line_comment"; i += 2; continue
            if c == "/" and nxt == "*":
                state = "block_comment"; i += 2; continue
            if c == '"' or c == "'":
                state = c; out.append(" "); i += 1; continue
            out.append(c); i += 1
        elif state == "line_comment":
            out.append(c if c == "\n" else " "); i += 1
            if c == "\n": state = "code"
        elif state == "block_comment":
            if c == "*" and nxt == "/":
                out.append("  "); i += 2; state = "code"
            else:
                out.append(c if c == "\n" else " "); i += 1
        else:  # string or char literal
            if c == "\\":
                out.append("  "); i += 2; continue
            if c == state:
                out.append(" "); i += 1; state = "code"
            else:
                out.append(c if c == "\n" else " "); i += 1
    return "".join(out)

def detect_func_name(code, brace_idx):
    j = brace_idx - 1
    while j >= 0 and code[j] in " \t\r\n":
        j -= 1
    if j < 0 or code[j] != ")":
        return None
    depth = 0
    k = j
    while k >= 0:
        if code[k] == ")":
            depth += 1
        elif code[k] == "(":
            depth -= 1
            if depth == 0:
                break
        k -= 1
    if k < 0:
        return None
    m = k - 1
    while m >= 0 and code[m] in " \t\r\n":
        m -= 1
    end = m + 1
    while m >= 0 and (code[m].isalnum() or code[m] == "_"):
        m -= 1
    name = code[m + 1:end]
    if not name or name in KEYWORDS:
        return None
    return name

def extract(path):
    try:
        with open(path) as fh:
            code = strip_c(fh.read())
    except OSError:
        return {}
    funcs = {}
    stack = []  # entries: (kind, enclosing_func_name|None)
    i, n = 0, len(code)
    while i < n:
        c = code[i]
        if c == "{":
            if not stack:
                name = detect_func_name(code, i)
                stack.append(("func" if name else "block", name))
                if name:
                    funcs[name] = 0
            else:
                stack.append(("block", stack[-1][1]))
            i += 1
        elif c == "}":
            if stack:
                stack.pop()
            i += 1
        else:
            m = ALLOC_RE.match(code, i)
            if m and stack and stack[-1][1]:
                funcs[stack[-1][1]] += 1
                i += len(m.group(0))
            else:
                i += 1
    return funcs

head, base = sys.argv[1], sys.argv[2]
h, b = extract(head), extract(base)
min_allocs = int(os.environ.get("MIN_ALLOCS", "2"))
for name in sorted(set(h) - set(b)):
    if h[name] >= min_allocs:
        print(name)
PY
    pyrc=$?
    set -e

    mapfile -t NEW_FUNCS < "$py_tmp"
    rm -f "$head_tmp" "$base_tmp" "$py_tmp"

    if [ "$pyrc" -ne 0 ]; then
        echo "error: fault-test parser failed for $f" >&2
        FAILURES=$((FAILURES + 1))
        continue
    fi
    if [ "${#NEW_FUNCS[@]}" -eq 0 ]; then
        continue
    fi

    basename="$(basename "$f" .c)"
    mapfile -t TEST_FILES < <(find tests -name "test_${basename}.c" 2>/dev/null || true)

    for fn in "${NEW_FUNCS[@]}"; do
        FOUND=$((FOUND + 1))
        if [ "${#TEST_FILES[@]}" -eq 0 ]; then
            echo "WARN: new multi-alloc function $fn in $f has no test file tests/**/test_${basename}.c"
            FAILURES=$((FAILURES + 1))
            continue
        fi
        covered=0
        for tf in "${TEST_FILES[@]}"; do
            if grep -qE "$MARKER_RE" "$tf"; then
                covered=1
                break
            fi
        done
        if [ "$covered" -eq 1 ]; then
            echo "ok:   new multi-alloc function $fn in $f is fault-tested (${TEST_FILES[*]})"
        else
            echo "WARN: new multi-alloc function $fn in $f: ${TEST_FILES[*]} exists but contains no fault-injection marker"
            FAILURES=$((FAILURES + 1))
        fi
    done
done

if [ "$FOUND" -eq 0 ]; then
    echo "fault-injection gate: no new multi-allocation functions vs $base — OK (mode: $MODE)"
elif [ "$MODE" = "blocking" ] && [ "$FAILURES" -gt 0 ]; then
    echo "fault-injection gate: FAILED ($FAILURES finding(s)) — new multi-allocation functions must ship a fault-injection test in the same PR (AGENTS.md 'Verification discipline')."
    exit 1
else
    echo "fault-injection gate: $FAILURES finding(s) (mode: $MODE, report-only since $REPORT_SINCE, flip target $FLIP_DATE)"
fi
