BUILD_DIR ?= build
CMAKE ?= cmake
CTEST ?= ctest
CONFIG ?= config.conf

.PHONY: all debug release clean test run run-web run-cli docs init

init:
	@if [ ! -f config.conf ]; then cp config.conf.example config.conf && echo "Created config.conf from example"; else echo "config.conf already exists"; fi

all: init debug

debug:
	$(CMAKE) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug
	$(CMAKE) --build $(BUILD_DIR)

release:
	$(CMAKE) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

test: debug
	$(CTEST) --test-dir $(BUILD_DIR) -V

run: run-web

run-web: debug
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
