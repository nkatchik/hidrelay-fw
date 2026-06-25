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
APP_SKIP_GIT_HOOKS_BOOTSTRAP ?=

.PHONY: help platform-list require-platform git-hooks-bootstrap bootstrap configure build release clean distclean sync-compile-commands tool-configure tool-cache-probe tool-diag-capture tool-diag-summary tool-diag-gate tool-diag-alert tool-app-replay test-host ci ci-host ci-diag-smoke ci-platform FORCE

help:
	@printf '%s\n' \
		'hidrelay-fw helper targets:' \
		'  make help              - Show this help text' \
		'  make platform-list     - List discovered platform targets' \
		'  make git-hooks-bootstrap   - Configure local git hooks path (.githooks)' \
		'  make bootstrap APP_PLATFORM=<target> - Download/cache SDK/toolchain for target' \
		'  make configure APP_PLATFORM=<target> - Generate CMake build tree (runs bootstrap first)' \
		'  make build APP_PLATFORM=<target>     - Build firmware for target' \
		'  make release                         - Build release artifacts for discovered platforms' \
		'  make release-<target>                - Build release artifact for one platform target' \
		'  platform/<target>/bin/*             - Platform-specific helper scripts (flash/tooling/etc.)' \
		'  make clean [APP_PLATFORM=<target>]   - Remove target build dir, or all build dirs when omitted' \
		'  make distclean         - Remove all local build/cache artifacts' \
		'  make tool-cache-probe  - Build host-side cleanup demonstration tool' \
		'  make tool-diag-capture - Build host-side CDC diagnostics capture tool' \
		'  make tool-diag-summary INPUT=diag.csv - Summarize captured diagnostics CSV' \
		'  make tool-diag-gate INPUT=diag.csv [MAX_RECONNECT_FAILURE_DELTA=n] - Enforce soak thresholds' \
		'  make tool-diag-alert INPUT=diag.csv [OUTPUT=diag_report.md] [MAX_RECONNECT_FAILURE_DELTA=n] - Generate markdown gate report' \
		'  make tool-app-replay   - Build host-side app replay validator' \
		'  make test-host         - Run host-side app replay validator' \
		'  make ci                - Run host checks and platform CI hooks'

platform-list:
	@for platform_dir in "$(APP_SOURCE_DIR)"/platform/*; do \
		if [ -d "$$platform_dir" ] && [ -f "$$platform_dir/CMakeLists.txt" ]; then \
			basename "$$platform_dir"; \
		fi; \
	done | LC_ALL=C sort

require-platform:
	@if [ -z "$(APP_PLATFORM)" ]; then \
		echo "APP_PLATFORM is required. Run 'make platform-list' and pass APP_PLATFORM=<target>."; \
		exit 2; \
	fi

git-hooks-bootstrap:
	@if [ -n "$(APP_SKIP_GIT_HOOKS_BOOTSTRAP)" ] || [ -n "$$CI" ]; then \
		echo 'Skipping git hooks bootstrap (CI/non-developer build)'; \
	elif command -v git >/dev/null 2>&1 && git rev-parse --is-inside-work-tree >/dev/null 2>&1; then \
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

release:
	@found=0; \
	rm -rf "$(BUILD_ROOT)/release"; \
	mkdir -p "$(BUILD_ROOT)/release"; \
	for platform_dir in "$(APP_SOURCE_DIR)"/platform/*; do \
		if [ ! -d "$$platform_dir" ] || [ ! -f "$$platform_dir/CMakeLists.txt" ]; then \
			continue; \
		fi; \
		platform=$$(basename "$$platform_dir"); \
		found=1; \
		$(MAKE) --no-print-directory release-$$platform; \
	done; \
	if [ "$$found" -eq 0 ]; then \
		echo "No platform targets found under platform/*"; \
		exit 2; \
	fi

release-%: FORCE
	@platform="$*"; \
	if [ ! -f "$(APP_SOURCE_DIR)/platform/$$platform/CMakeLists.txt" ]; then \
		echo "Unknown release platform '$$platform'. Run 'make platform-list'."; \
		exit 2; \
	fi; \
	mkdir -p "$(BUILD_ROOT)/release"; \
	asset_platform=$$(printf '%s' "$$platform" | tr '_' '-'); \
	build_dir="$(BUILD_ROOT)/release-build/$$platform"; \
	stage_dir="$(BUILD_ROOT)/release-stage/$$platform"; \
	$(MAKE) --no-print-directory bootstrap APP_PLATFORM=$$platform APP_SKIP_GIT_HOOKS_BOOTSTRAP=1; \
	$(CMAKE_ENV) $(CMAKE) -S "$(APP_SOURCE_DIR)" -B "$$build_dir" \
		-DAPP_PLATFORM=$$platform \
		-DAPP_CACHE_DIR="$(APP_CACHE_DIR)" \
		-DCMAKE_TOOLCHAIN_FILE="$(APP_CACHE_DIR)/toolchain/$$platform.cmake" \
		-DCMAKE_BUILD_TYPE=Release \
		-DAPP_PLATFORM_ENABLE_TINYUSB:BOOL=ON \
		-DAPP_PLATFORM_ENABLE_BTSTACK:BOOL=ON \
		-DAPP_PLATFORM_ENABLE_TELEMETRY:BOOL=OFF \
		-DAPP_PLATFORM_ENABLE_DIAG_CDC:BOOL=OFF \
		-DAPP_PLATFORM_DEBUG_WIPE_ALL_ON_BOOT:BOOL=OFF \
		-C "$(APP_CACHE_DIR)/bootstrap/$$platform.cmake"; \
	$(CMAKE_ENV) $(CMAKE) --build "$$build_dir" --parallel; \
	uf2_path="$$build_dir/platform/$$platform/hidrelay_fw.uf2"; \
	if [ -f "$$uf2_path" ]; then \
		asset_path="$(BUILD_ROOT)/release/hidrelay-fw-$$asset_platform.uf2"; \
		cp "$$uf2_path" "$$asset_path"; \
		printf 'release artifact: %s\n' "$$asset_path"; \
		exit 0; \
	fi; \
	if [ -f "$$build_dir/bootloader/bootloader.bin" ] \
		&& [ -f "$$build_dir/partition_table/partition-table.bin" ] \
		&& [ -f "$$build_dir/hidrelay_fw.bin" ]; then \
		chip=$$(printf '%s' "$$asset_platform" | tr -d '-'); \
		rm -rf "$$stage_dir"; \
		mkdir -p "$$stage_dir"; \
		cp "$$build_dir/bootloader/bootloader.bin" "$$stage_dir/bootloader.bin"; \
		cp "$$build_dir/partition_table/partition-table.bin" "$$stage_dir/partition-table.bin"; \
		cp "$$build_dir/hidrelay_fw.bin" "$$stage_dir/hidrelay_fw.bin"; \
		printf 'esptool.py --chip %s write_flash \\\n' "$$chip" \
			> "$$stage_dir/flash_args.txt"; \
		printf '%s\n' \
			'  0x0 bootloader.bin \' \
			'  0x8000 partition-table.bin \' \
			'  0x10000 hidrelay_fw.bin' \
			>> "$$stage_dir/flash_args.txt"; \
		asset_path="$(BUILD_ROOT)/release/hidrelay-fw-$$asset_platform.zip"; \
		$(CMAKE) -E rm -f "$$asset_path"; \
		(cd "$$stage_dir" && $(CMAKE) -E tar cf "$$asset_path" --format=zip \
			bootloader.bin partition-table.bin hidrelay_fw.bin flash_args.txt); \
		printf 'release artifact: %s\n' "$$asset_path"; \
		exit 0; \
	fi; \
	echo "No known release artifact layout for platform '$$platform'"; \
	exit 2

FORCE:

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

ci: ci-host ci-platform

ci-host: test-host tool-cache-probe tool-diag-capture ci-diag-smoke

ci-diag-smoke:
	@./tool/bin/diag_smoke

ci-platform:
	@found=0; \
	for hook in "$(APP_SOURCE_DIR)"/platform/*/bin/ci-build; do \
		if [ ! -f "$$hook" ]; then \
			continue; \
		fi; \
		found=1; \
		"$$hook"; \
	done; \
	if [ "$$found" -eq 0 ]; then \
		echo "No platform CI hooks found under platform/*/bin/ci-build"; \
		exit 2; \
	fi

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
