#!/usr/bin/env bash
# debug-check.sh - run a single Check test suite under a debugger or
# valgrind. AGENTS.md rule 79: with CK_FORK=no the test runs in-process,
# so gdb/valgrind can see the failure; CK_RUN_SUITE / CK_RUN_CASE /
# CK_INCLUDE_TAGS isolate one test instead of commenting code out.
#
# Usage:
#   scripts/debug-check.sh gdb      build/tests/tools/test_bash
#   scripts/debug-check.sh valgrind build/tests/agent/test_session_manager
#   scripts/debug-check.sh run      build/tests/...  (plain, CK_FORK=no)
set -eu

mode="${1:?usage: debug-check.sh <gdb|valgrind|run> <test-binary> [args...]}"
shift
binary="${1:?usage: debug-check.sh <gdb|valgrind|run> <test-binary>}"
shift

if [ ! -x "$binary" ]; then
    echo "error: test binary not found: $binary (build it first)" >&2
    exit 1
fi

case "$mode" in
    gdb)
        CK_FORK=no gdb -ex run --args "$binary" "$@"
        ;;
    valgrind)
        CK_FORK=no valgrind --leak-check=full --error-exitcode=1 \
            "$binary" "$@"
        ;;
    run)
        CK_FORK=no "$binary" "$@"
        ;;
    *)
        echo "error: mode must be gdb, valgrind, or run" >&2
        exit 1
        ;;
esac
