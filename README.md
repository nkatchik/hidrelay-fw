# hidrelay-fw

Bare-metal firmware skeleton for an open source Bluetooth HID to USB HID hub.

Primary target: **Raspberry Pi Pico W**.

This repository is intentionally conservative: it focuses on build/bootstrap structure and clean module boundaries, with incremental BTstack/TinyUSB integration and stubbed HID bridge behavior.

## Status

Current implementation is a buildable skeleton with:

- local toolchain and SDK bootstrap
- platform-selectable CMake layout (`APP_PLATFORM` required)
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
- auth failure path now emits explicit security-rotation requests to platform stack (current Pico W implementation revokes stored key/bonding state for target device)
- operator command surface now requires challenge-response sessions over CDC (`AUTH HELLO`/`AUTH PROVE` + signed `CMD` frames), enforces monotonic command sequence checks, applies auth lockout (5 failures -> 30s cooldown), and keeps 500ms command acceptance rate limiting
- shared HID report-descriptor policy with extended sanitization checks (global stack balance, report-id limits, bounded field sizes, required input/application items)
- per-interface TinyUSB report descriptor export from BTstack HID descriptor storage with deterministic fallback selection (native, boot keyboard, boot mouse, generic)
- descriptor remap now covers boot fallback profiles with BT<->USB report-id/payload normalization, including boot-keyboard LED output translation
- explicit SSP/PIN confirmation handling gated by pairing mode
- optional runtime telemetry surfaces (structured snapshots + stdio mirror) enabled in debug/dev builds (`APP_PLATFORM_ENABLE_TELEMETRY`)
- optional host-visible diagnostics transport over TinyUSB CDC with framed binary snapshot streaming (`APP_PLATFORM_ENABLE_DIAG_CDC`, requires telemetry)
- release guardrails reject telemetry/diagnostics in `Release` builds unless explicitly overridden (`APP_PLATFORM_ALLOW_RELEASE_TELEMETRY=ON`)
- queue backpressure telemetry with drop counters/high-water marks
- host-side deterministic app replay validator (`make test-host`) for reconnect/button/queue regression checks without hardware soak
- host-side diagnostics gate report generator for inbox/alert workflows (`make tool-diag-alert`)
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
make bootstrap APP_PLATFORM=pico_w
make build APP_PLATFORM=pico_w
```

Run host-side deterministic validation (no board required):

```sh
make test-host
```

Default Pico W firmware artifacts are written to `build/pico_w/platform/pico_w/`, including `hidrelay_fw.uf2`.

`make bootstrap` now also configures local git hooks (`core.hooksPath=.githooks`) when run inside a git worktree.

Show discovered targets:

```sh
make platform-list
```

`APP_PLATFORM` is required. Build with:

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
  -DAPP_PLATFORM_ENABLE_DIAG_CDC=ON \
  -DAPP_PLATFORM_OPERATOR_AUTH_KEY_HEX=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff
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

Release guardrails will fail configure if telemetry/diagnostics are enabled in `Release`. For explicit development-only override:

```sh
cmake -S . -B build/pico_w_release_devdiag \
  -DAPP_PLATFORM=pico_w \
  -DCMAKE_BUILD_TYPE=Release \
  -DAPP_PLATFORM_ENABLE_TINYUSB=ON \
  -DAPP_PLATFORM_ENABLE_BTSTACK=ON \
  -DAPP_PLATFORM_ENABLE_TELEMETRY=ON \
  -DAPP_PLATFORM_ENABLE_DIAG_CDC=ON \
  -DAPP_PLATFORM_OPERATOR_AUTH_KEY_HEX=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff \
  -DAPP_PLATFORM_ALLOW_RELEASE_TELEMETRY=ON
cmake --build build/pico_w_release_devdiag --parallel
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
- auth reconnect failures now raise a security-rotation request hook that platform consumes for key/bond refresh
- TinyUSB report descriptors are exported per interface from live BT HID descriptor storage when available
- descriptor acceptance/fallback now runs through shared policy checks, with boot-profile fallback descriptors for incompatible boot-mode reports
- boot-profile fallback paths now apply report remap normalization in Pico stack TX/RX (keyboard/mouse payload + report-id shaping, including keyboard LED output handling)
- BTstack key material and LE device records persist via TLV flash storage
- BT security events (PIN/SSP confirmation) are explicitly handled according to pairing state
- one queued report per tick is forwarded in each direction via `usb_bridge`
- queue saturation drops oldest pending reports and updates telemetry counters
- when `APP_PLATFORM_ENABLE_TELEMETRY=ON`, diagnostics snapshots are mirrored to stdio and exposed via `platform_diag_take(...)`
- when `APP_PLATFORM_ENABLE_TELEMETRY=ON` and `APP_PLATFORM_ENABLE_DIAG_CDC=ON`, diagnostics snapshots are additionally published over TinyUSB CDC interface `0`
- diagnostics now include Pico stack event-queue telemetry (depth/high-water/drop counters) for dropped-event visibility
- operator command ingress now requires challenge-response session auth configured by `APP_PLATFORM_OPERATOR_AUTH_KEY_HEX`
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
make tool-diag-alert INPUT=diag.csv OUTPUT=diag_report.md
```

`make tool-diag-gate` exits non-zero when drop deltas are non-zero. Optionally add reconnect-failure gating:

```sh
make tool-diag-gate INPUT=diag.csv MAX_RECONNECT_FAILURE_DELTA=0
```

Generate a markdown-ready gate report for inbox/alert pipelines:

```sh
make tool-diag-alert INPUT=diag.csv OUTPUT=diag_report.md MAX_RECONNECT_FAILURE_DELTA=0
```

When diagnostics CDC is enabled, the same CDC interface accepts operator recovery via authenticated session commands:

- start challenge: `AUTH HELLO <client_nonce_hex_16>`
- complete auth: `AUTH PROVE <session_id_hex_8> <proof_mac_hex_64>`
- signed command: `CMD <session_id_hex_8> <sequence_dec> <LOCKOUT_CLEAR_ALL|LOCKOUT_CLEAR_LAST|ROTATE_LAST> <command_mac_hex_64>`

Operator commands are rate-limited to one accepted command every `500ms`.
After `5` auth failures, operator command acceptance is locked for `30s`.
Diagnostics CDC builds require `-DAPP_PLATFORM_OPERATOR_AUTH_KEY_HEX=<64 hex chars>`.

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
- add host-side operator client tooling for challenge-response signing and key rotation workflows
- extend descriptor remap coverage beyond current boot-profile + keyboard-LED handling into broader host edge-case translation paths
- wire `tool-diag-alert` output into your CI/inbox notification path for soak-gate failures (without runtime telemetry in release builds)
