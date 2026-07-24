BUILD_DIR ?= build
CMAKE   ?= cmake
CTEST   ?= ctest
CONFIG  ?= config.conf
NPM     ?= npm
NPM_DIR  = frontend

.PHONY: all debug release clean test run run-web run-cli docs init frontend frontend-build frontend-test frontend-clean

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

run: run-web

run-web: frontend-build debug
	$(BUILD_DIR)/echo-ai --web --config $(CONFIG)

run-cli: debug
	$(BUILD_DIR)/echo-ai --cli --config $(CONFIG)

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
