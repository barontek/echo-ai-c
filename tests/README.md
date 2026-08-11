# tests — Check unit tests, fuzz targets, and fixtures

What this owns: 61 Check test binaries (one per source module, mirrored
1:1 with `add_test`), 16 libFuzzer targets for every external-input
parser, and the shared stub files used to isolate modules from real
providers, uv loops, and SQLite where a unit test needs them.

Why it exists: AGENTS.md's testing rules require sanitizer-clean runs,
fault-injection (allocation-failure) coverage for every multi-alloc
commit path, and a regression test for every bug fix. This tree is the
proof of those rules: see `tests/*/CMakeLists.txt` for the compile
definitions that opt test targets into the `*_TEST` guards (production
builds never see them).

Layout: `tests/` mirrors `src/` — `test_<module>.c` per module, fuzz
targets named `fuzz_<module>.c`. Run everything with `ctest` from the
build directory, or a single suite with
`ctest -R test_<module> --output-on-failure`.
