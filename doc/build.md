# Build and Bootstrap

## Requirements

Assumed globally installed tools:

- `gcc`
- `make`
- `cmake`
- standard CLI tools (`sh`, `git`, `curl`, `tar`, `unzip`)

No global Pico SDK, BTstack, TinyUSB, or Arm cross-toolchain install is required.

## Commands

```sh
make bootstrap APP_PLATFORM=pico_w
make build APP_PLATFORM=pico_w
```

Useful targets:

- `make help`
- `make platform-list`
- `make bootstrap APP_PLATFORM=<target>`
- `make configure APP_PLATFORM=<target>`
- `make build APP_PLATFORM=<target>`
- `make clean [APP_PLATFORM=<target>]`
- `make distclean`
- `make tool-cache-probe`
- `make tool-diag-capture`
- `make tool-diag-summary INPUT=diag.csv`
- `make tool-diag-gate INPUT=diag.csv [MAX_RECONNECT_FAILURE_DELTA=n]`
- `make tool-app-replay`
- `make test-host`

## Cached Artifacts

Bootstrap stores downloads/checkouts under `.cache/`:

- `.cache/download/` - downloaded archives
- `.cache/tool/` - extracted Arm toolchain
- `.cache/sdk/` - Pico SDK checkout
- `.cache/toolchain/` - generated toolchain file(s)
- `.cache/bootstrap/` - generated CMake cache seed file(s)

Build trees are under `build/<platform>/`.

For default Pico W builds, outputs are generated under `build/pico_w/platform/pico_w/`, including:

- `hidrelay_fw.elf`
- `hidrelay_fw.bin`
- `hidrelay_fw.uf2`
- `hidrelay_fw.map`
- `hidrelay_fw.dis`

## Deterministic Pins

Current pinned versions for the Pico W platform:

- Arm GNU toolchain: `13.2.Rel1`
- Pico SDK git tag: `2.0.0`

Update these in:

- `platform/pico_w/bootstrap.cmake`

## Platform Model

Platform is selected by CMake cache variable:

- `APP_PLATFORM` (required; run `make platform-list` to see available targets)

To add a new platform:

1. add `platform/<new_target>/CMakeLists.txt`
2. add `platform/<new_target>/bootstrap.cmake`
3. add `platform/<new_target>/src/...` for `platform_api` implementation
4. optionally add `platform/<new_target>/toolchain.cmake.in`
5. optionally add `platform/<new_target>/cmake/*.cmake` helpers

Pico W stack toggles:

- `APP_PLATFORM_ENABLE_TINYUSB` (default `OFF`)
- `APP_PLATFORM_ENABLE_BTSTACK` (default `OFF`)
- `APP_PLATFORM_ENABLE_TELEMETRY` (default `ON` for `Debug`, otherwise `OFF`)
- `APP_PLATFORM_ENABLE_DIAG_CDC` (default follows `APP_PLATFORM_ENABLE_TELEMETRY`; requires TinyUSB + telemetry)
- `APP_PLATFORM_ALLOW_RELEASE_TELEMETRY` (default `OFF`; required to permit telemetry/diag options with `CMAKE_BUILD_TYPE=Release`)

They are disabled by default for fast baseline builds. Project-local starter TinyUSB/BTstack headers are provided for stack-enabled development builds.

Stack-enabled build example:

```sh
cmake -S . -B build/pico_w_stack \
  -DAPP_PLATFORM=pico_w \
  -DAPP_PLATFORM_ENABLE_TINYUSB=ON \
  -DAPP_PLATFORM_ENABLE_BTSTACK=ON
cmake --build build/pico_w_stack --parallel
```

Debug/development stack build with diagnostics CDC:

```sh
cmake -S . -B build/pico_w_debug \
  -DAPP_PLATFORM=pico_w \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAPP_PLATFORM_ENABLE_TINYUSB=ON \
  -DAPP_PLATFORM_ENABLE_BTSTACK=ON \
  -DAPP_PLATFORM_ENABLE_TELEMETRY=ON \
  -DAPP_PLATFORM_ENABLE_DIAG_CDC=ON
cmake --build build/pico_w_debug --parallel
```

Release guardrail example (expected configure failure):

```sh
cmake -S . -B build/pico_w_release_guard \
  -DAPP_PLATFORM=pico_w \
  -DCMAKE_BUILD_TYPE=Release \
  -DAPP_PLATFORM_ENABLE_TELEMETRY=ON
```

Explicit development-only override for release-like builds:

```sh
cmake -S . -B build/pico_w_release_devdiag \
  -DAPP_PLATFORM=pico_w \
  -DCMAKE_BUILD_TYPE=Release \
  -DAPP_PLATFORM_ENABLE_TINYUSB=ON \
  -DAPP_PLATFORM_ENABLE_BTSTACK=ON \
  -DAPP_PLATFORM_ENABLE_TELEMETRY=ON \
  -DAPP_PLATFORM_ENABLE_DIAG_CDC=ON \
  -DAPP_PLATFORM_ALLOW_RELEASE_TELEMETRY=ON
cmake --build build/pico_w_release_devdiag --parallel
```

Project-local stack config headers used by this path:

- `platform/pico_w/include/tusb_config.h`
- `platform/pico_w/include/btstack_config.h`

UF2 output is required in this repository's default workflow; `PICO_NO_PICOTOOL=ON` is not supported.
Pico SDK/picotool compatibility defaults are platform-owned in `platform/pico_w/cmake/options.cmake`: it sets `CMAKE_POLICY_DEFAULT_CMP0169=OLD` and defaults `PICOTOOL_FETCH_FROM_GIT_PATH` to `${APP_CACHE_DIR}/picotool`.

## Warning Policy

Project-owned targets compile with:

- `-Wall`
- `-Wextra`
- `-Wpedantic`

Configured in `cmake/CommonOptions.cmake`.

## Notes on Current Scope

This pass intentionally does not implement full HID bridging yet.

Implemented now:

- module boundaries
- build/bootstrap workflow
- compileable stubs
- BOOTSEL command FSM semantics and LED command indications
- optional BTstack/TinyUSB stack-enabled Pico W build path
- BT manager active HID session model and event-ingest API (`bt_manager_ingest_hid_open/close`)
- USB bridge interface-plan model with descriptor-generation tracking
- dynamic TinyUSB HID configuration descriptor composition from interface count
- platform stack USB-plan handoff and TinyUSB runtime isolation (`platform_pico_w_tinyusb_runtime.*`)
- BTstack HID open/close/report event ingestion into app transport events
- BTstack HID descriptor/protocol event ingestion into app transport events
- TinyUSB output-report callback ingestion into app transport events
- bidirectional queued report forwarding with protocol-aware BT send path
- queue backpressure telemetry (depth/high-water/drop counters) in `usb_bridge`
- flash-backed Pair DB load/save on Pico W (`platform_pico_w_pair_store.*`) with session metadata schema v4 and dual-slot A/B journal (two sectors reserved ahead of BTstack TLV banks)
- Pair DB save path now suppresses no-op writes and uses sequence-based latest-slot selection
- main loop coalesces Pair DB persistence writes (2s debounce, 15s max stale, 5s retry backoff) to reduce flash wear
- BTstack TLV-backed persistence for classic link keys and LE device database
- pair-any inquiry/connect flow gated by pairing state and class-of-device policy
- reconnect retry policy with per-device backoff windows and timeout-based failure classification
- reconnect outcome signaling from platform stack (stack reject/connect/auth result classes)
- reconnect policy handling by failure class (transient stack-reject retry, connect-failure backoff, auth-failure timed lockout)
- reconnect escalation threshold now applies timed lockout with automatic recovery after repeated connect/timeout failures
- shared HID report-descriptor policy checks (global stack push/pop balance, report-id limits, field bounds, required input/application items)
- per-interface TinyUSB report descriptor export from BTstack HID descriptor storage with deterministic fallback profiles (native, boot keyboard, boot mouse, generic)
- initial descriptor remap groundwork for boot fallback profiles (BT<->USB report-id/payload normalization in stack TX/RX paths)
- explicit BTstack PIN/SSP confirmation handling policy tied to pairing-mode state
- runtime bridge/pairing diagnostics emitted on state change via stdio log lines when `APP_PLATFORM_ENABLE_TELEMETRY=ON` (including reconnect counters/result + status code)
- structured diagnostics dequeue API exposed at platform boundary (`platform_diag_take`) when telemetry is enabled
- optional TinyUSB CDC diagnostics function (HID interfaces + CDC control/data pair) gated by `APP_PLATFORM_ENABLE_DIAG_CDC`
- when telemetry+CDC are enabled, diagnostics snapshots are streamed over TinyUSB CDC as framed binary records (magic/version/payload + sequence) including Pico stack event-queue depth/high-water/drop counters
- remove-last command now emits a per-device forget request from app to platform, and Pico W stack drops link-key/bonding state for that device
- factory reset command now erases Pair DB + BTstack persisted security material and reboots after cue completion
- host-side CDC diagnostics capture utility (`build/tool/diag_capture`) outputs decoded CSV frames
- host-side diagnostics summary helper (`tool/bin/diag_summary`) reports soak max/delta counters from CSV captures
- host-side diagnostics gate mode now enforces thresholds and exits non-zero for automation (`make tool-diag-gate`)
- host-side deterministic app replay validator (`build/tool/app_replay`) covers button command mapping, reconnect scheduling/lockout recovery, and queue overflow behavior (`make test-host`)
- release build guardrails reject telemetry/diagnostics in `Release` unless explicitly overridden (`APP_PLATFORM_ALLOW_RELEASE_TELEMETRY=ON`)
- soak capture/trend runbook documented in `doc/soak.md`

Still pending for production behavior:

- reconnect retry policy tuning using long-run telemetry and deployment data
- Bluetooth key migration/rotation and explicit operator recovery controls beyond current per-device forget/factory reset
- descriptor translation/remapping for host edge cases beyond current boot-profile groundwork
- alerting/inbox workflow integration layered on top of soak diagnostics gate failures
- command UX refinement and full failure-recovery handling
