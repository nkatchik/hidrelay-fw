SHELL := /bin/sh

# Keep this root Makefile platform-agnostic.
# Platform-specific tooling belongs in platform/<target>/bin.

APP_SOURCE_DIR := $(CURDIR)
APP_PLATFORM ?=
APP_CACHE_DIR := $(APP_SOURCE_DIR)/.cache
BUILD_ROOT ?= $(APP_SOURCE_DIR)/build
BUILD_DIR := $(BUILD_ROOT)/$(APP_PLATFORM)
BOOTSTRAP_CACHE := $(APP_CACHE_DIR)/bootstrap/$(APP_PLATFORM).cmake
TOOLCHAIN_FILE := $(APP_CACHE_DIR)/toolchain/$(APP_PLATFORM).cmake
COMPILE_COMMANDS_SOURCE := $(BUILD_DIR)/compile_commands.json
COMPILE_COMMANDS_LINK := $(BUILD_ROOT)/compile_commands.json
TOOL_SOURCE_DIR := $(APP_SOURCE_DIR)/tool
TOOL_BUILD_DIR := $(BUILD_ROOT)/tool
TOOL_COMPILE_COMMANDS_SOURCE := $(TOOL_BUILD_DIR)/compile_commands.json
CMAKE_POLICY_VERSION_MINIMUM ?= 3.5
CMAKE_ENV := CMAKE_POLICY_VERSION_MINIMUM=$(CMAKE_POLICY_VERSION_MINIMUM)

CMAKE ?= cmake
HOST_CC ?= gcc
APP_DEBUG_WIPE_ALL_ON_BOOT ?=

.PHONY: help platform-list require-platform git-hooks-bootstrap bootstrap configure build clean distclean sync-compile-commands tool-configure tool-cache-probe tool-diag-capture tool-diag-summary tool-diag-gate tool-diag-alert tool-app-replay test-host

help:
	@printf '%s\n' \
		'hidrelay-fw helper targets:' \
		'  make help              - Show this help text' \
		'  make platform-list     - List discovered platform targets' \
		'  make git-hooks-bootstrap   - Configure local git hooks path (.githooks)' \
		'  make bootstrap APP_PLATFORM=<target> - Download/cache SDK/toolchain for target' \
		'  make configure APP_PLATFORM=<target> - Generate CMake build tree (runs bootstrap first)' \
		'  make build APP_PLATFORM=<target>     - Build firmware for target' \
		'  platform/<target>/bin/*             - Platform-specific helper scripts (flash/tooling/etc.)' \
		'  make clean [APP_PLATFORM=<target>]   - Remove target build dir, or all build dirs when omitted' \
		'  make distclean         - Remove all local build/cache artifacts' \
		'  make tool-cache-probe  - Build host-side cleanup demonstration tool' \
		'  make tool-diag-capture - Build host-side CDC diagnostics capture tool' \
		'  make tool-diag-summary INPUT=diag.csv - Summarize captured diagnostics CSV' \
		'  make tool-diag-gate INPUT=diag.csv [MAX_RECONNECT_FAILURE_DELTA=n] - Enforce soak thresholds' \
		'  make tool-diag-alert INPUT=diag.csv [OUTPUT=diag_report.md] [MAX_RECONNECT_FAILURE_DELTA=n] - Generate markdown gate report' \
		'  make tool-app-replay   - Build host-side app replay validator' \
		'  make test-host         - Run host-side app replay validator'

platform-list:
	@find "$(APP_SOURCE_DIR)/platform" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; | LC_ALL=C sort

require-platform:
	@if [ -z "$(APP_PLATFORM)" ]; then \
		echo "APP_PLATFORM is required. Run 'make platform-list' and pass APP_PLATFORM=<target>."; \
		exit 2; \
	fi

git-hooks-bootstrap:
	@if command -v git >/dev/null 2>&1 && git rev-parse --is-inside-work-tree >/dev/null 2>&1; then \
		$(APP_SOURCE_DIR)/.githooks/bootstrap; \
	else \
		echo 'Skipping git hooks bootstrap (not a git worktree)'; \
	fi

bootstrap: require-platform git-hooks-bootstrap
	@$(CMAKE) \
		-DAPP_SOURCE_DIR=$(APP_SOURCE_DIR) \
		-DAPP_PLATFORM=$(APP_PLATFORM) \
		-P $(APP_SOURCE_DIR)/cmake/Bootstrap.cmake

configure: require-platform bootstrap
	@$(CMAKE_ENV) $(CMAKE) -S $(APP_SOURCE_DIR) -B $(BUILD_DIR) \
		-DAPP_PLATFORM=$(APP_PLATFORM) \
		-DAPP_CACHE_DIR=$(APP_CACHE_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE) \
		$(if $(APP_DEBUG_WIPE_ALL_ON_BOOT),-DAPP_PLATFORM_DEBUG_WIPE_ALL_ON_BOOT=$(APP_DEBUG_WIPE_ALL_ON_BOOT),) \
		-C $(BOOTSTRAP_CACHE)
	@$(MAKE) --no-print-directory sync-compile-commands APP_PLATFORM=$(APP_PLATFORM)

build: require-platform configure
	@$(CMAKE_ENV) $(CMAKE) --build $(BUILD_DIR) --parallel

clean:
	@if [ -n "$(APP_PLATFORM)" ]; then \
		rm -rf "$(BUILD_DIR)"; \
	else \
		rm -rf "$(BUILD_ROOT)"; \
	fi

distclean:
	@rm -rf $(BUILD_ROOT) $(APP_CACHE_DIR)

sync-compile-commands: require-platform
	@if [ ! -f "$(COMPILE_COMMANDS_SOURCE)" ]; then \
		echo "Missing $(COMPILE_COMMANDS_SOURCE). Run make configure APP_PLATFORM=$(APP_PLATFORM) first."; \
		exit 1; \
	fi
	@$(MAKE) --no-print-directory tool-configure
	@if [ ! -f "$(TOOL_COMPILE_COMMANDS_SOURCE)" ]; then \
		echo "Missing $(TOOL_COMPILE_COMMANDS_SOURCE). Tool CMake configure failed."; \
		exit 1; \
	fi
	tmp_file="$(COMPILE_COMMANDS_LINK).tmp"; \
	jq -s \
		'.[0] as $$fw \
		| .[1] as $$host \
		| ($$fw | map(.file)) as $$fw_files \
		| ($$fw + [ $$host[] | select((.file as $$f | ($$fw_files | index($$f))) | not) ])' \
		"$(COMPILE_COMMANDS_SOURCE)" "$(TOOL_COMPILE_COMMANDS_SOURCE)" \
		> "$$tmp_file" && mv "$$tmp_file" "$(COMPILE_COMMANDS_LINK)"
	@echo "Using compile commands: $(COMPILE_COMMANDS_LINK)"

tool-configure:
	@$(CMAKE) -S $(TOOL_SOURCE_DIR) -B $(TOOL_BUILD_DIR) -DCMAKE_C_COMPILER=$(HOST_CC)

tool-cache-probe:
	@$(MAKE) --no-print-directory tool-configure
	@$(CMAKE) --build $(TOOL_BUILD_DIR) --target cache_probe --parallel

tool-diag-capture:
	@$(MAKE) --no-print-directory tool-configure
	@$(CMAKE) --build $(TOOL_BUILD_DIR) --target diag_capture --parallel

tool-app-replay:
	@$(MAKE) --no-print-directory tool-configure
	@$(CMAKE) --build $(TOOL_BUILD_DIR) --target app_replay --parallel

test-host: tool-app-replay
	@$(TOOL_BUILD_DIR)/app_replay

tool-diag-summary:
	@if [ -z "$(INPUT)" ]; then \
		echo 'Usage: make tool-diag-summary INPUT=diag.csv'; \
		exit 2; \
	fi
	@./tool/bin/diag_summary --input "$(INPUT)"

tool-diag-gate:
	@if [ -z "$(INPUT)" ]; then \
		echo 'Usage: make tool-diag-gate INPUT=diag.csv [MAX_RECONNECT_FAILURE_DELTA=n]'; \
		exit 2; \
	fi
	@if [ -n "$(MAX_RECONNECT_FAILURE_DELTA)" ]; then \
		./tool/bin/diag_summary --input "$(INPUT)" --require-no-drops \
			--max-reconnect-failure-delta "$(MAX_RECONNECT_FAILURE_DELTA)"; \
	else \
		./tool/bin/diag_summary --input "$(INPUT)" --require-no-drops; \
	fi

tool-diag-alert:
	@if [ -z "$(INPUT)" ]; then \
		echo 'Usage: make tool-diag-alert INPUT=diag.csv [OUTPUT=diag_report.md] [MAX_RECONNECT_FAILURE_DELTA=n]'; \
		exit 2; \
	fi
	@if [ -n "$(MAX_RECONNECT_FAILURE_DELTA)" ]; then \
		if [ -n "$(OUTPUT)" ]; then \
			./tool/bin/diag_alert --input "$(INPUT)" --output "$(OUTPUT)" \
				--max-reconnect-failure-delta "$(MAX_RECONNECT_FAILURE_DELTA)"; \
		else \
			./tool/bin/diag_alert --input "$(INPUT)" \
				--max-reconnect-failure-delta "$(MAX_RECONNECT_FAILURE_DELTA)"; \
		fi; \
	else \
		if [ -n "$(OUTPUT)" ]; then \
			./tool/bin/diag_alert --input "$(INPUT)" --output "$(OUTPUT)"; \
		else \
			./tool/bin/diag_alert --input "$(INPUT)"; \
		fi; \
	fi
