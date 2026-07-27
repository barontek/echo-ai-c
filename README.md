# Echo AI

Lean, self-contained C rewrite of the Echo AI agentic system. Single binary, minimal dependencies, vanilla JS frontend.

[![CI](https://github.com/barontek/echo-ai-c/actions/workflows/ci.yml/badge.svg)](https://github.com/barontek/echo-ai-c/actions/workflows/ci.yml)

## Quick Start

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cp config.conf.example config.conf
# edit config.conf to set agent.model
./build/echo-ai
```

Open http://localhost:8080 in your browser.

## Dependencies

| Library | Use |
|---------|-----|
| libuv | Event loop + TCP for HTTP/WS server |
| libcurl | HTTP client for Ollama, web_fetch, web_search |
| sqlite3 | Session DB, rate limiter |
| openssl | AES-128-CBC, HMAC-SHA256, Scrypt, SHA1 |
| cJSON | JSON parse/serialize |

### Install

```bash
# Nix (recommended — works on Linux and macOS)
nix develop

# Debian/Ubuntu
apt install cmake pkg-config libuv1-dev libcurl4-openssl-dev libsqlite3-dev libssl-dev libcjson-dev check

# Fedora
dnf install cmake pkg-config libuv-devel libcurl-devel sqlite-devel openssl-devel libcjson-devel check

# Arch
pacman -S cmake pkg-config libuv curl sqlite openssl cjson check

# macOS (Homebrew)
brew install cmake pkg-config libuv curl openssl sqlite cjson check
export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig:$(brew --prefix)/opt/openssl/lib/pkgconfig"
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# note: on macOS, ASan leak detection is disabled — set ASAN_OPTIONS=detect_leaks=0 at runtime
```

## CLI Flags

| Flag | Description |
|------|-------------|
| `--web` | HTTP server on port 8080 (default) |
| `--cli` | Interactive REPL with rich-rendered chat |
| `--chat` | Lightweight interactive chat (not yet implemented) |
| `--config PATH` | Path to config file (default: `config.conf`) |
| `--debug` | Enable debug-level logging |
| `--help` | Show help message |

## Testing

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build -V
```

## Configuration

Custom `.conf` format (see `config.conf.example`):

```conf
# comment
key = value

[section]
nested_key = value
```

## API Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | /api/status | No | Server status (locked/setup/ready) |
| GET | /api/health | No | Health check |
| GET | /api/config | No | Public config (models, providers) |
| POST | /api/setup | No | First-run setup (set password) |
| POST | /api/unlock | No | Unlock with password |
| POST | /api/logout | Token | Invalidate unlock token |
| GET | /api/sessions | Token | List sessions |
| POST | /api/sessions | Token | Create session |
| GET | /api/sessions/:id | Token | Get session with messages |
| DELETE | /api/sessions/:id | Token | Delete session |
| PUT | /api/sessions/:id | Token | Update session title |
| POST | /api/chat | Token | Send message (REST) |
| GET | /api/stream | No | SSE streaming |
| GET | /api/metrics | No | Prometheus metrics |
| POST | /api/undo | Token | Undo last file write |
| POST | /api/redo | Token | Redo last undone file write |
| GET /ws/chat | WebSocket | No | Real-time chat |

## Environment Variables

| Variable | Description |
|----------|-------------|
| `ECHO_PASSWORD` | Encryption password for session persistence |

## Project Structure

```
src/
├── main.c              Entry point, arg parsing
├── agent/              Agent loop, messages, context
├── llm/                LLM provider (Ollama)
├── tools/              Tool system (bash, file ops, search, etc.)
├── server/             HTTP/WS server (libuv)
├── session/            SQLite session store + encryption
├── safety/             Path validation, approval, safety config
├── config/             .conf parser
├── change_tracker/     File undo/redo
├── utils/              Logging, metrics, circuit breaker, rate limiter, strings, etc.
web/
├── index.html          SPA shell
├── css/style.css       Dark/light theme
├── js/                 app.js, chat.js, websocket.js, api.js, components.js
```
