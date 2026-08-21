#!/usr/bin/env bash
# lint.sh - clang-tidy static-analysis gate (runs in the CI
# static-analysis job; also runnable locally via `make lint`).
#
# clang-tidy replays the CMake compile commands with clang, but the
# commands only carry the cjson -isystem flag: the rest of the include
# search path comes from the gcc wrapper's hidden flags. So the wrapper's
# real search list (glibc, curl, openssl, ...) is extracted from
# `gcc -v` and handed to clang-tidy verbatim. gcc's own internal dirs
# (/lib/gcc/..., include-fixed) are excluded: their stdatomic.h is a
# freestanding wrapper that shadows clang's proper resource headers.
#
# Usage: scripts/lint.sh [file.c ...]   (default: every tracked src TU)
#
# Each TU runs in its own clang-tidy process (no -j flag exists), so
# xargs -P provides parallelism and PER_FILE_TIMEOUT (default 300s) via
# the coreutils timeout(1) time-boxes each one — a pathological TU is
# reported and skipped, never allowed to hang the run.

set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
PER_FILE_TIMEOUT="${PER_FILE_TIMEOUT:-300}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    echo "error: $BUILD_DIR/compile_commands.json missing — configure first:" >&2
    echo "  cmake -B $BUILD_DIR -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
    exit 1
fi

INC_FILE="$(mktemp)"
trap 'rm -f "$INC_FILE" "$SUMMARY_FILE"' EXIT
while IFS= read -r line; do
    case "$line" in
        */lib/gcc/*|*/include-fixed*) ;; # gcc internal: shadow clang's own headers
        *) printf '%s\n' "$line" >> "$INC_FILE" ;;
    esac
done < <(gcc -v -E -x c /dev/null -o /dev/null 2>&1 \
    | sed -n '/#include <...> search starts here:/,/End of search list/p' \
    | sed 's/^ //' \
    | grep '^/')

if [ "$#" -gt 0 ]; then
    FILES=("$@")
else
    mapfile -t FILES < <(git ls-files 'src/*.c')
fi

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "error: no files to lint" >&2
    exit 1
fi

SUMMARY_FILE="$(mktemp)"

lint_one() {
    local f="$1"
    local out
    out="$(mktemp)"
    local extra=()
    local d
    while IFS= read -r d; do
        extra+=(--extra-arg-before=-isystem --extra-arg-before="$d")
    done < "$INC_FILE"
    if timeout "${PER_FILE_TIMEOUT}s" \
        clang-tidy -p "$BUILD_DIR" "${extra[@]}" "$f" >"$out" 2>&1; then
        # clang-tidy may still exit 0 with findings (WarningsAsErrors is
        # config/version-dependent); a diagnostic line is a finding
        # regardless of exit code.
        if grep -qE ':[0-9]+:[0-9]+: (warning|error):' "$out"; then
            echo "fail $f" >> "$SUMMARY_FILE"
            cat "$out"
        else
            echo "ok $f" >> "$SUMMARY_FILE"
        fi
        rm -f "$out"
        return 0
    fi
    local rc=$?
    if [ "$rc" -eq 124 ]; then
        echo "timeout $f" >> "$SUMMARY_FILE"
        echo "warning: $f hit the ${PER_FILE_TIMEOUT}s file cap — skipped" >&2
    else
        echo "fail $f" >> "$SUMMARY_FILE"
        cat "$out"
    fi
    rm -f "$out"
    return 0
}
export -f lint_one
export BUILD_DIR INC_FILE PER_FILE_TIMEOUT SUMMARY_FILE

echo "linting ${#FILES[@]} translation units (${JOBS} jobs, ${PER_FILE_TIMEOUT}s/file cap)..."
printf '%s\n' "${FILES[@]}" \
    | xargs -P "$JOBS" -n 1 bash -c 'lint_one "$0"'

echo "---"
cat "$SUMMARY_FILE"
fails="$(grep -c '^fail' "$SUMMARY_FILE" || true)"
times="$(grep -c '^timeout' "$SUMMARY_FILE" || true)"
echo "${#FILES[@]} files: $(( ${#FILES[@]} - fails - times )) ok, $fails failed, $times timed out"
[ "$fails" -eq 0 ] && [ "$times" -eq 0 ]
