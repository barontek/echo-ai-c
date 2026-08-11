BUILD_DIR ?= build
CMAKE   ?= cmake
CTEST   ?= ctest
CONFIG  ?= config.conf
NPM     ?= npm
NPM_DIR  = frontend

.PHONY: all debug release clean test run run-web run-cli run-tls docs init frontend frontend-build frontend-test frontend-clean

init:
	@if [ ! -f config.conf ]; then cp config.conf.example config.conf && echo "Created config.conf from example"; else echo "config.conf already exists"; fi

all: init frontend-build debug

debug:
	$(CMAKE) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug
	$(CMAKE) --build $(BUILD_DIR)

release:
	$(CMAKE) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD_DIR)

clean: frontend-clean
	rm -rf $(BUILD_DIR)

test: debug
	$(CTEST) --test-dir $(BUILD_DIR) -V

# G1: debugger/valgrind helpers for a single Check suite (see
# scripts/debug-check.sh — uses CK_FORK=no + CK_RUN_* isolation).
check-debug:
	@echo "usage: make check-debug BIN=build/tests/... MODE=gdb|valgrind|run"
	@[ -n "$(BIN)" ] && scripts/debug-check.sh $(MODE) $(BIN) || true

valgrind:
	@echo "usage: make valgrind BIN=build/tests/..."
	@[ -n "$(BIN)" ] && scripts/debug-check.sh valgrind $(BIN) || true

run: run-web

run-web: frontend-build debug
	$(BUILD_DIR)/echo-ai --web --config $(CONFIG)

run-cli: debug
	$(BUILD_DIR)/echo-ai --cli --config $(CONFIG)

# echo-ai behind a Caddy TLS proxy (see deploy/Caddyfile). Needs `caddy`
# on PATH (nix develop on Nix; `apt install caddy` / `brew install caddy`
# elsewhere). Ctrl-C stops both processes.
run-tls: debug
	@./scripts/serve-tls.sh

docs:
	@echo "=== README ==="
	@head -5 README.md
	@echo ""
	@echo "=== Man page ==="
	@man ./echo-ai.1 2>/dev/null || nroff -man echo-ai.1 2>/dev/null | head -10
	@echo ""
	@echo "=== Config example ==="
	@head -5 config.conf.example

# --- frontend ---

frontend: frontend-build

frontend-build/install:
	cd $(NPM_DIR) && $(NPM) install

frontend-build: frontend-build/install
	cd $(NPM_DIR) && $(NPM) run build

frontend-test:
	cd $(NPM_DIR) && $(NPM) test

frontend-clean:
	rm -rf $(NPM_DIR)/dist

# --- code coverage ---
COVERAGE_DIR ?= build-coverage

coverage:
	$(CMAKE) -B $(COVERAGE_DIR) -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON -DENABLE_SANITIZERS=OFF
	$(CMAKE) --build $(COVERAGE_DIR)
	$(CTEST) --test-dir $(COVERAGE_DIR) -V || true
	gcovr -r . --object-directory=$(COVERAGE_DIR) --print-summary --sort-percentage --decisions
	gcovr -r . --object-directory=$(COVERAGE_DIR) --html-details coverage.html --html-title "Echo AI Coverage"

coverage-clean:
	rm -rf $(COVERAGE_DIR) coverage.html coverage.*.html
