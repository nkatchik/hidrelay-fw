SHELL := /bin/sh

APP_SOURCE_DIR := $(CURDIR)
APP_PLATFORM ?= $(shell find "$(APP_SOURCE_DIR)/platform" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; | LC_ALL=C sort | head -n 1)
APP_CACHE_DIR := $(APP_SOURCE_DIR)/.cache
BUILD_ROOT ?= $(APP_SOURCE_DIR)/build
BUILD_DIR := $(BUILD_ROOT)/$(APP_PLATFORM)
BOOTSTRAP_CACHE := $(APP_CACHE_DIR)/bootstrap/$(APP_PLATFORM).cmake
TOOLCHAIN_FILE := $(APP_CACHE_DIR)/toolchain/$(APP_PLATFORM).cmake
COMPILE_COMMANDS_SOURCE := $(BUILD_DIR)/compile_commands.json
COMPILE_COMMANDS_LINK := $(BUILD_ROOT)/compile_commands.json
PICO_NO_PICOTOOL ?= OFF
CMAKE_POLICY_VERSION_MINIMUM ?= 3.5
CMAKE_ENV := CMAKE_POLICY_VERSION_MINIMUM=$(CMAKE_POLICY_VERSION_MINIMUM)

CMAKE ?= cmake
HOST_CC ?= gcc

.PHONY: help platform-list git-hooks-bootstrap bootstrap configure build clean distclean sync-compile-commands tool-cache-probe tool-diag-capture tool-diag-summary tool-diag-gate

help:
	@printf '%s\n' \
		'hidrelay-fw helper targets:' \
		'  make help              - Show this help text' \
		'  make platform-list     - List discovered platform targets' \
		'  make git-hooks-bootstrap   - Configure local git hooks path (.githooks)' \
		'  make bootstrap         - Download/cache SDK/toolchain for APP_PLATFORM' \
		'  make configure         - Generate CMake build tree (runs bootstrap first)' \
		'  make build             - Build firmware for APP_PLATFORM' \
		'  make clean             - Remove build directory for APP_PLATFORM' \
		'  make distclean         - Remove all local build/cache artifacts' \
		'  make tool-cache-probe  - Build host-side cleanup demonstration tool' \
		'  make tool-diag-capture - Build host-side CDC diagnostics capture tool' \
		'  make tool-diag-summary INPUT=diag.csv - Summarize captured diagnostics CSV' \
		'  make tool-diag-gate INPUT=diag.csv [MAX_RECONNECT_FAILURE_DELTA=n] - Enforce soak thresholds'

platform-list:
	@find "$(APP_SOURCE_DIR)/platform" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; | LC_ALL=C sort

git-hooks-bootstrap:
	@if command -v git >/dev/null 2>&1 && git rev-parse --is-inside-work-tree >/dev/null 2>&1; then \
		$(APP_SOURCE_DIR)/.githooks/bootstrap; \
	else \
		echo 'Skipping git hooks bootstrap (not a git worktree)'; \
	fi

bootstrap: git-hooks-bootstrap
	@if [ -z "$(APP_PLATFORM)" ]; then \
		echo 'No platform target found under platform/'; \
		exit 2; \
	fi
	@$(CMAKE) \
		-DAPP_SOURCE_DIR=$(APP_SOURCE_DIR) \
		-DAPP_PLATFORM=$(APP_PLATFORM) \
		-P $(APP_SOURCE_DIR)/cmake/Bootstrap.cmake

configure: bootstrap
	@$(CMAKE_ENV) $(CMAKE) -S $(APP_SOURCE_DIR) -B $(BUILD_DIR) \
		-DAPP_PLATFORM=$(APP_PLATFORM) \
		-DAPP_CACHE_DIR=$(APP_CACHE_DIR) \
		-DPICO_NO_PICOTOOL=$(PICO_NO_PICOTOOL) \
		-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE) \
		-C $(BOOTSTRAP_CACHE)
	@$(MAKE) --no-print-directory sync-compile-commands APP_PLATFORM=$(APP_PLATFORM)

build: configure
	@$(CMAKE_ENV) $(CMAKE) --build $(BUILD_DIR) --parallel

clean:
	@rm -rf $(BUILD_DIR)

distclean:
	@rm -rf $(BUILD_ROOT) $(APP_CACHE_DIR)

sync-compile-commands:
	@if [ ! -f "$(COMPILE_COMMANDS_SOURCE)" ]; then \
		echo "Missing $(COMPILE_COMMANDS_SOURCE). Run make configure APP_PLATFORM=$(APP_PLATFORM) first."; \
		exit 1; \
	fi
	@ln -sfn "$(COMPILE_COMMANDS_SOURCE)" "$(COMPILE_COMMANDS_LINK)"
	@echo "Using compile commands: $(COMPILE_COMMANDS_SOURCE)"

tool-cache-probe: build/tool/cache_probe

tool-diag-capture: build/tool/diag_capture

tool-diag-summary:
	@if [ -z "$(INPUT)" ]; then \
		echo 'Usage: make tool-diag-summary INPUT=diag.csv'; \
		exit 2; \
	fi
	@./tool/diag_summary.sh --input "$(INPUT)"

tool-diag-gate:
	@if [ -z "$(INPUT)" ]; then \
		echo 'Usage: make tool-diag-gate INPUT=diag.csv [MAX_RECONNECT_FAILURE_DELTA=n]'; \
		exit 2; \
	fi
	@if [ -n "$(MAX_RECONNECT_FAILURE_DELTA)" ]; then \
		./tool/diag_summary.sh --input "$(INPUT)" --require-no-drops \
			--max-reconnect-failure-delta "$(MAX_RECONNECT_FAILURE_DELTA)"; \
	else \
		./tool/diag_summary.sh --input "$(INPUT)" --require-no-drops; \
	fi

build/tool/cache_probe: tool/cache_probe.c src/util/cleanup.c include/util/cleanup.h
	@mkdir -p build/tool
	@$(HOST_CC) -std=c17 -Wall -Wextra -Wpedantic -Iinclude \
		tool/cache_probe.c src/util/cleanup.c -o build/tool/cache_probe

build/tool/diag_capture: tool/diag_capture.c src/util/cleanup.c include/util/cleanup.h
	@mkdir -p build/tool
	@$(HOST_CC) -std=c17 -Wall -Wextra -Wpedantic -Iinclude \
		tool/diag_capture.c src/util/cleanup.c -o build/tool/diag_capture
