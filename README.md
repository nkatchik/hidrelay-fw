# hidrelay-fw

Bare-metal firmware skeleton for an open source Bluetooth HID to USB HID hub.

Default target: **Raspberry Pi Pico W**.

This repository is intentionally conservative: it focuses on build/bootstrap structure and clean module boundaries, with incremental BTstack/TinyUSB integration and stubbed HID bridge behavior.

## Status

Current implementation is a buildable skeleton with:

- local toolchain and SDK bootstrap
- platform-selectable CMake layout (auto-discovers from `platform/<name>/`)
- app event loop and module boundaries for future HID bridging
- optional Pico W stack bring-up for BTstack + TinyUSB with local config headers
- BT manager active-HID session model with BTstack event ingestion (`bt_manager_ingest_hid_*`)
- USB bridge interface-plan model with descriptor generation tracking and bounded report queues
- TinyUSB configuration descriptor composition for 0..8 HID interfaces
- bidirectional report path skeleton (`BT HID report` -> USB IN, `USB OUT report` -> BT HID protocol-aware send)
- pair-any discovery/connect flow using BT inquiry and HID connect (pairing mode gated)
- per-device protocol/descriptor metadata propagation and protocol-aware BT report send path
- reconnect policy with multi-device candidate selection, per-device backoff, and timeout-based failure classification
- explicit reconnect outcome signaling from platform stack (stack-reject/connect-failed/auth-failed classes)
- reconnect retry policy now branches by failure class (transient stack reject, connect failure timeout/backoff, auth failure timed lockout)
- reconnect escalation threshold now applies timed lockout with automatic recovery instead of permanent disable
- shared HID report-descriptor policy with extended sanitization checks (global stack balance, report-id limits, bounded field sizes, required input/application items)
- per-interface TinyUSB report descriptor export from BTstack HID descriptor storage with deterministic fallback selection (native, boot keyboard, boot mouse, generic)
- explicit SSP/PIN confirmation handling gated by pairing mode
- optional runtime telemetry surfaces (structured snapshots + stdio mirror) enabled in debug/dev builds (`APP_PLATFORM_ENABLE_TELEMETRY`)
- optional host-visible diagnostics transport over TinyUSB CDC with framed binary snapshot streaming (`APP_PLATFORM_ENABLE_DIAG_CDC`, requires telemetry)
- queue backpressure telemetry with drop counters/high-water marks
- BTstack TLV-backed key persistence for classic link keys and LE device DB
- flash-backed pair database persistence with session metadata (schema v4, dual-slot A/B journal ahead of BTstack flash banks)
- coalesced Pair DB save policy in main loop (2s debounce, 15s max stale window, 5s retry backoff) to reduce flash wear under bursty updates
- factory reset path that erases Pair DB and BTstack key material from flash after the 3-blink cue, then reboots
- remove-last flow now issues per-device forget requests into platform stack so link keys/bonding state are revoked for that device
- BOOTSEL button command FSM for:
  - pair-any
  - remove-last
  - remove-all

## Quick Start

```sh
make bootstrap
make build
```

Default Pico W firmware artifacts are written to `build/pico_w/platform/pico_w/`, including `hidrelay_fw.uf2`.

`make bootstrap` now also configures local git hooks (`core.hooksPath=.githooks`) when run inside a git worktree.

Show discovered targets:

```sh
make platform-list
```

The default target is the first discovered folder in `platform/` (currently `pico_w`). Override with:

```sh
make APP_PLATFORM=pico_w build
```

Enable Pico SDK stack linkage (not required for skeleton build):

```sh
cmake -S . -B build/pico_w \
  -DAPP_PLATFORM=pico_w \
  -DAPP_PLATFORM_ENABLE_TINYUSB=ON \
  -DAPP_PLATFORM_ENABLE_BTSTACK=ON
cmake --build build/pico_w --parallel
```

Enable CDC diagnostics transport in debug/development builds:

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

Release builds keep telemetry disabled by default:

```sh
cmake -S . -B build/pico_w_release \
  -DAPP_PLATFORM=pico_w \
  -DCMAKE_BUILD_TYPE=Release \
  -DAPP_PLATFORM_ENABLE_TINYUSB=ON \
  -DAPP_PLATFORM_ENABLE_BTSTACK=ON
cmake --build build/pico_w_release --parallel
```

These options remain off by default for fast baseline iteration, but the repository now includes working starter configs in:

- `platform/pico_w/include/tusb_config.h`
- `platform/pico_w/include/btstack_config.h`

When stack options are enabled:

- `platform_pico_w_stack` initializes BTstack and TinyUSB
- BTstack HID open/close/report events are bridged into common app transport events
- BTstack HID descriptor/protocol events are bridged into common app transport events
- TinyUSB `set_report` callbacks are bridged into common app transport events
- pair-any state drives BT inquiry/connection attempts with class-of-device filtering
- TinyUSB descriptor callbacks build configuration descriptors from the current interface plan
- app reconnect requests now run through per-device backoff windows and timeout tracking
- reconnect result events are emitted from stack paths (immediate reject/connect/auth outcomes)
- app reconnect failure handling now applies per-result retry policy updates with cooldown-based auto-recovery windows
- TinyUSB report descriptors are exported per interface from live BT HID descriptor storage when available
- descriptor acceptance/fallback now runs through shared policy checks, with boot-profile fallback descriptors for incompatible boot-mode reports
- BTstack key material and LE device records persist via TLV flash storage
- BT security events (PIN/SSP confirmation) are explicitly handled according to pairing state
- one queued report per tick is forwarded in each direction via `usb_bridge`
- queue saturation drops oldest pending reports and updates telemetry counters
- when `APP_PLATFORM_ENABLE_TELEMETRY=ON`, diagnostics snapshots are mirrored to stdio and exposed via `platform_diag_take(...)`
- when `APP_PLATFORM_ENABLE_TELEMETRY=ON` and `APP_PLATFORM_ENABLE_DIAG_CDC=ON`, diagnostics snapshots are additionally published over TinyUSB CDC interface `0`
- diagnostics now include Pico stack event-queue telemetry (depth/high-water/drop counters) for dropped-event visibility
- Pair DB save path now suppresses no-op writes and alternates flash slots using sequence-based latest selection
- remove-last now also requests platform-side BT security cleanup for that specific device (link key/bonding records)
- factory reset now clears Pair DB + BTstack persisted security data and triggers reboot

## Repository Layout

- `cmake/` - bootstrap and build modules
- `doc/` - architecture and build documentation
- `include/` - public headers for app/platform modules
- `src/` - platform-agnostic firmware logic
- `platform/` - platform-specific glue (`platform/pico_w/` now)
- `tool/` - host-side helper code (`cache_probe`, `diag_capture`)

Generated/downloaded artifacts are local and git-ignored:

- `build/`
- `.cache/`

## BOOTSEL Command Mapping

Current mapping:

- long press (>= 2s): `pair-any` (fires after a 2.5s double-long window)
- double long press (two >= 2s presses): `remove-last` only when the last paired device is at most 1 hour old; otherwise no-op
- very long press (>= 10s): `remove-all` / factory reset

LED behavior:

- pairing mode blinks at 1Hz while active (up to 60s timeout)
- remove-last success: one long blink
- factory reset: three long blinks
- after the three blinks complete, firmware erases all persisted pairing/security state and reboots

## Diagnostics CDC Frame

When TinyUSB is enabled and both `APP_PLATFORM_ENABLE_TELEMETRY=ON` and `APP_PLATFORM_ENABLE_DIAG_CDC=ON`, diagnostics snapshots are streamed on CDC interface `0` as binary frames:

- byte `0`: magic `'H'` (`0x48`)
- byte `1`: magic `'R'` (`0x52`)
- byte `2`: frame version (`1`)
- byte `3`: payload length (`39`)
- bytes `4..`: little-endian payload:
  - `u32 sequence`
  - `u8 bt_state`
  - `u8 active_device_count`
  - `u8 usb_interface_count`
  - `u8 usb_tx_depth`
  - `u8 bt_tx_depth`
  - `u8 usb_tx_high_watermark`
  - `u8 bt_tx_high_watermark`
  - `u8 reconnect_last_result`
  - `u8 reconnect_last_status_code`
  - `u32 usb_tx_dropped`
  - `u32 bt_tx_dropped`
  - `u32 reconnect_attempt_count`
  - `u32 reconnect_success_count`
  - `u32 reconnect_failure_count`
  - `u8 stack_event_depth`
  - `u8 stack_event_high_watermark`
  - `u32 stack_event_dropped`

Capture and decode frames to CSV on host:

```sh
make tool-diag-capture
build/tool/diag_capture --device /dev/tty.usbmodemXXXX --baud 115200 --output diag.csv
make tool-diag-summary INPUT=diag.csv
make tool-diag-gate INPUT=diag.csv
```

`make tool-diag-gate` exits non-zero when drop deltas are non-zero. Optionally add reconnect-failure gating:

```sh
make tool-diag-gate INPUT=diag.csv MAX_RECONNECT_FAILURE_DELTA=0
```

## Coding Rules

Project-owned C code uses `__attribute__((cleanup(...)))` for resource disposal.

- No explicit `free`, `fclose`, or `close` outside cleanup helper internals.
- Cleanup helpers are defined in `include/util/cleanup.h` and `src/util/cleanup.c`.

## Next Work

See:

- `doc/architecture.md`
- `doc/build.md`
- `doc/soak.md`

Next implementation steps:

- tune reconnect policy thresholds/escalation with long-run device telemetry
- extend per-device security lifecycle controls with explicit migration/rotation and operator recovery paths
- extend descriptor handling from policy-only fallback into explicit report translation/remapping for host edge cases
- add alerting/inbox workflow integration around soak gate failures (without runtime telemetry in release builds)
