# Echo AI — Agent Guide

## Project Overview

Echo AI is an agentic AI system with a conversational frontend and a tool-using backend. This is a C rewrite following `ARCHITECTURE.md`.

## Build & Development

### Dev shell
```bash
nix develop
```

### Build & test
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build -V
```

### Dependencies (runtime)
| Library | Use |
|---|---|
| libuv | Event loop + TCP for HTTP/WS server |
| libcurl | HTTP client for Ollama, web_fetch, web_search |
| sqlite3 | Session DB, rate limiter |
| openssl | AES-128-CBC, HMAC-SHA256, Scrypt, SHA1 |
| cJSON | JSON parse/serialize |

Testing: Check (dev only).

Compiler flags: `-Wall -Wextra -Wpedantic -Werror -g` (Debug), `-Wall -Wextra -O2` (Release).

## Code Conventions

### Style
- **C11** standard
- **4-space** indent, no tabs
- **K&R brace style**: opening brace on same line for functions, `{` on same line for control flow
- **Spaces**: space before `{` in control flow (`if (cond) {`), no space before `(` in function calls (`foo()`)
- **No comments in code** —  the codebase contains no comments beyond headers and example files
- **Line length**: soft limit of ~100 chars

### Naming
- `lowercase_snake_case` for functions, variables, file names
- `UpperCamelCase` for types, struct names
- `ECHO_SCREAMING_SNAKE` for macros, enum values
- Module prefixes: `conf_*`, `log_*`, `str_*`, `msg_*`, `tool_*`, `agent_*`, etc.
- Header guards: `ECHO_<MODULE>_H`

### Memory
- Manual `malloc`/`free` throughout
- `calloc` for struct allocation (zero-init)
- `str_dup` wrapper instead of POSIX `strdup` (defined in `string_utils.h`)
- `asprintf` for dynamic string building (GNU extension, needs `_GNU_SOURCE`)
- Always free on error paths; NULL-check allocations

### Includes
- `_GNU_SOURCE` defined before any includes (for `asprintf`, POSIX extensions)
- Standard library includes first, then project headers
- Internal includes use relative paths from `src/`: `"config/config.h"`, `"../utils/string_utils.h"`

### Error handling
- Return NULL/error code on failure, never abort or assert
- Log errors with `log_error()` before returning
- Check all allocation returns, all `fopen`, all `asprintf`

## Logging

Structured JSON to stderr:

```c
log_info("starting echo-ai", "mode", "cli", NULL);
log_error("failed to load config", "path", "/foo/bar", NULL);
```

Arguments are key-value pairs terminated by NULL. Levels: `debug`, `info`, `warn`, `error`.

Macros: `log_debug()`, `log_info()`, `log_warn()`, `log_error()` —  prepend `__FILE__` and `__LINE__` automatically.

## Configuration

Custom `.conf` format — not YAML:

```conf
# comment
key = value

[section]
nested_key = value
```

Accessed as `section.nested_key` via `conf_get(conf, "section.nested_key")`.

## Testing

Check framework. Each test file is standalone (has its own `main()`):

```c
#include <check.h>

START_TEST(test_name)
{
    ck_assert_int_eq(foo(), 42);
}
END_TEST

Suite *suite(void) { ... }
int main(void) { srunner_run_all(srunner_create(suite()), CK_NORMAL); ... }
```

Test files live in `tests/`, listed in `tests/CMakeLists.txt`.

## Project Structure

```
src/
├── main.c              Entry point, arg parsing
├── config/             .conf parser
├── agent/              Agent loop, messages, context
├── llm/                LLM provider interface + implementations
├── tools/              Tool system (registry + individual tools)
├── server/             HTTP/WS server (libuv)
├── session/            SQLite session store + encryption
├── safety/             Path validation, approval, safety config
├── utils/              Logging, strings, JSON, HTTP client, metrics, etc.
├── change_tracker/     File undo/redo
```

## Useful Commands

```bash
nix develop                                          # enter dev shell
cmake -B build -DCMAKE_BUILD_TYPE=Debug              # configure
cmake --build build                                  # compile
ctest --test-dir build -V                            # run tests
./build/echo-ai --help                               # run
./build/echo-ai --config myconfig.conf --cli          # custom config + CLI mode
```
