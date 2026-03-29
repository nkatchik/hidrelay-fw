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
make bootstrap
make build
```

Useful targets:

- `make help`
- `make platform-list`
- `make bootstrap`
- `make configure`
- `make build`
- `make clean`
- `make distclean`
- `make tool-cache-probe`

## Cached Artifacts

Bootstrap stores downloads/checkouts under `.cache/`:

- `.cache/download/` - downloaded archives
- `.cache/tool/` - extracted Arm toolchain
- `.cache/sdk/` - Pico SDK checkout
- `.cache/toolchain/` - generated toolchain file(s)
- `.cache/bootstrap/` - generated CMake cache seed file(s)

Build trees are under `build/<platform>/`.

For default Pico W builds, outputs are generated under `build/pico_w/platform/pico_w/`, including:

- `hidrelay_fw` (ELF)
- `hidrelay_fw.bin`
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

- `APP_PLATFORM` (default: `auto`, which resolves to the first discovered folder under `platform/`)

To add a new platform:

1. add `platform/<new_target>/CMakeLists.txt`
2. add `platform/<new_target>/bootstrap.cmake`
3. add `platform/<new_target>/src/...` for `platform_api` implementation
4. optionally add `platform/<new_target>/toolchain.cmake.in`
5. optionally add `platform/<new_target>/cmake/*.cmake` helpers

Pico W stack toggles:

- `APP_PLATFORM_ENABLE_TINYUSB` (default `OFF`)
- `APP_PLATFORM_ENABLE_BTSTACK` (default `OFF`)

They are disabled by default for fast baseline builds. Project-local starter TinyUSB/BTstack headers are provided for stack-enabled development builds.

Stack-enabled build example:

```sh
cmake -S . -B build/pico_w_stack \
  -DAPP_PLATFORM=pico_w \
  -DAPP_PLATFORM_ENABLE_TINYUSB=ON \
  -DAPP_PLATFORM_ENABLE_BTSTACK=ON
cmake --build build/pico_w_stack --parallel
```

Project-local stack config headers used by this path:

- `platform/pico_w/include/tusb_config.h`
- `platform/pico_w/include/btstack_config.h`

`PICO_NO_PICOTOOL` is enabled by default to avoid extra host-tool dependencies during initial skeleton work.

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
- flash-backed Pair DB load/save on Pico W (`platform_pico_w_pair_store.*`) with session metadata schema v3 (sector reserved ahead of BTstack TLV banks)
- BTstack TLV-backed persistence for classic link keys and LE device database
- pair-any inquiry/connect flow gated by pairing state and class-of-device policy
- reconnect retry policy with per-device backoff windows and timeout-based failure classification
- reconnect outcome signaling from platform stack (stack reject/connect/auth result classes)
- reconnect policy handling by failure class (transient stack-reject retry, connect-failure backoff, auth-failure disable)
- reconnect escalation threshold disables auto-reconnect after repeated connect/timeout failures
- per-interface TinyUSB report descriptor export from BTstack HID descriptor storage (structural sanitization + generic fallback)
- explicit BTstack PIN/SSP confirmation handling policy tied to pairing-mode state
- runtime bridge/pairing diagnostics emitted on state change via stdio log lines (including reconnect counters/result + status code)
- structured diagnostics dequeue API exposed at platform boundary (`platform_diag_take`)

Still pending for production behavior:

- per-device USB HID descriptor translation/sanitization policy beyond structural guardrails
- host-visible diagnostics transport for structured queue output (USB CDC/vendor endpoint)
- reconnect retry policy tuning using long-run telemetry and deployment data
- Bluetooth key migration/rotation and recovery controls
- command UX refinement and full failure-recovery handling
