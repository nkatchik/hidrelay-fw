# Architecture

## Goal

Bridge Bluetooth HID peripherals to a USB host computer, exposing one USB HID interface per connected Bluetooth HID device.

This repository currently provides the foundational architecture and a buildable firmware skeleton.

## Layering

- `src/`: shared firmware logic and state machines
- `platform/<name>/`: hardware/SDK glue
- `cmake/`: platform discovery, bootstrap dispatch, warning policy
- `tool/`: optional host-side helpers

Common logic never imports Pico-specific SDK headers.

## Core Modules

- `app`:
  - owns the main app state and event loop logic
  - wires the button FSM, LED UI, pair DB, BT manager, and USB bridge stubs
  - emits reconnect requests, per-device forget requests, and diagnostic snapshots as part of platform output
- `button_fsm`:
  - translates BOOTSEL press patterns into high-level commands (`pair-any`, `remove-last`, `remove-all`)
- `led_ui`:
  - provides user-facing LED state behavior independent from GPIO details
- `pair_db`:
  - paired-device store abstraction with paired timestamp and last-session metadata (descriptor length, protocol mode, vendor/product IDs, reconnect flag)
  - stores last-session transport hints (`Classic`/`LE` and LE address type) for reconnect path prioritization
  - tracks reconnect failure/backoff metadata per device for retry scheduling and timed lockout recovery
  - persisted on Pico W in a flash-backed blob through platform pair-store hooks
- `bt_manager`:
  - Bluetooth management API with pairing lifecycle and active HID session model
  - exposes event-ingest hooks for HID open/close/descriptor/protocol updates
- `usb_bridge`:
  - USB-facing interface plan derived from active BT HID sessions
  - carries BT link type metadata (Classic vs LE) per active session/interface slot
  - tracks descriptor generation for dynamic USB descriptor rebuild triggers
  - holds bounded routing queues for BT->USB and USB->BT HID reports
  - tracks queue depth/high-water/drop telemetry for backpressure visibility
- `tool/app_replay`:
  - deterministic host-side replay harness for app-loop regression checks without target hardware
  - validates button-command mapping, reconnect scheduling/lockout recovery, and queue overflow semantics
- `hid_report_policy`:
  - shared HID report-descriptor acceptance/fallback policy
  - enforces structural and compatibility guardrails before descriptor exposure to USB host
  - classifies fallback profile (boot keyboard, boot mouse, generic) when native descriptors are rejected
- `hid_report_remap`:
  - shared report remap helpers used when fallback descriptors are active
  - currently normalizes boot keyboard/mouse payload and report-id shaping for BT->USB and USB->BT paths
- `hid_device_map`:
  - device-profile detection hooks for mapping behavior by VID/PID
  - currently tracks Apple Magic Keyboard `Fn+Esc` mode toggles as bridge-side state for future remap policy extensions
- `platform_api`:
  - platform boundary: init, poll inputs, apply outputs
  - persistence hooks: `platform_pair_db_load` / `platform_pair_db_save`
  - structured diagnostics dequeue hook: `platform_diag_take`

## Pico W Platform Glue

`platform/pico_w/` contains:

- platform-local bootstrap/toolchain scripts:
  - `bootstrap.cmake`
  - `toolchain.cmake.in`
  - `cmake/options.cmake`
  - `cmake/target.cmake`
- Pico SDK import/init wiring
- `platform_api` implementation for:
  - CYW43 status LED
  - BOOTSEL button input
  - tick pacing via `sleep_ms`
- split runtime glue modules:
  - `platform_pico_w_state.*`
  - `platform_pico_w_hw.*`
  - `platform_pico_w_pair_store.*` (flash-backed Pair DB load/save)
  - `platform_pico_w_stack.*` (TinyUSB/BTstack bring-up hooks + USB plan handoff)
  - `platform_pico_w_tinyusb_runtime.*` (TinyUSB runtime calls isolated from BTstack includes)
  - `platform_pico_w_tinyusb_desc.c` (dynamic HID configuration descriptor callbacks)
- platform-local stack config headers:
  - `include/tusb_config.h`
  - `include/btstack_config.h`
- Pico SDK stack linkage flags:
  - `APP_PLATFORM_ENABLE_TINYUSB`
  - `APP_PLATFORM_ENABLE_BTSTACK`
  - `APP_PLATFORM_ENABLE_TELEMETRY` (debug/development diagnostics surfaces)
  - `APP_PLATFORM_ENABLE_DIAG_CDC` (optional debug/development CDC diagnostics transport)
  - `APP_PLATFORM_ALLOW_RELEASE_TELEMETRY` (explicit escape hatch for release-like development builds)

Pico-specific linkage is isolated under this directory.

## Current Integration Milestone

- TinyUSB stack is enabled by default for Pico W and built with a baseline HID device configuration.
- BTstack libraries are enabled by default for Pico W using project-local `btstack_config.h`.
- Platform runtime initializes Pico W stacks from `platform_pico_w_stack`.
- Common `bt_manager` now models active HID sessions, not only pair count.
- `usb_bridge` composes a per-interface plan from active sessions and increments descriptor generation on topology changes.
- TinyUSB descriptor callbacks now build a configuration descriptor dynamically (0..8 HID interfaces).
- TinyUSB runtime now performs controlled disconnect/reconnect re-enumeration when USB descriptor generation changes.
- BTstack HID open/close/report events are now translated into app transport events.
- BTstack HID descriptor/protocol events are now translated into app transport events.
- TinyUSB output report callbacks are now translated into app transport events.
- App and bridge now route queued reports in both directions (one dequeued report per direction per tick) with protocol-aware BT transmission.
- Pair-any mode now drives real BT inquiry/connect attempts under a class-of-device filter and pairing-mode gating.
- Pair-any mode now also scans BLE advertisements in active-scan mode, preferring HID service UUID (`0x1812`) candidates but allowing connectable+HID-appearance fallback so BLE-only accessories that omit UUID in some packets can still be discovered without opening to arbitrary connectable devices.
- App now derives per-interface USB descriptor/protocol hints from active sessions and emits them with each platform output.
- Active-session transport contract now includes BT link type so `hid_cid` routing remains deterministic across Classic and LE stacks.
- App now emits reconnect requests from persisted Pair DB metadata when idle, with per-device backoff windows.
- Reconnect requests now include last-session transport/address hints from Pair DB metadata.
- Platform stack can consume reconnect requests and invoke BT HID reconnect attempts.
- Reconnect path now attempts fallback stages for unknown transport history (`Classic -> LE public -> LE random`).
- Platform stack can consume per-device forget requests to disconnect current HID sessions and revoke persisted BT key/bonding state for that device.
- App reconnect policy now applies per-device backoff windows and timeout-based failure classification.
- Platform stack now emits reconnect result events for immediate reject/connect/auth outcomes.
- App reconnect policy now applies per-result handling (transient stack reject retry, connect-failure backoff, auth-failure timed lockout).
- App reconnect policy now escalates to timed reconnect lockout after repeated connect/timeout failures, with automatic recovery when cooldown expires.
- TinyUSB report descriptor callbacks now use shared descriptor policy checks (collection/global-stack validation, report-id limits, bounded field sizes, required input/application collections).
- Descriptor export now applies deterministic fallback selection (native, boot keyboard, boot mouse, generic) per interface.
- Descriptor export/source lookup now branches by active link type (Classic HID descriptor storage vs BLE HIDS descriptor storage).
- Boot fallback profiles now feed report remap helpers so keyboard/mouse payload shape matches fallback descriptor expectations in both directions, including boot-keyboard LED output translation.
- USB bridge now carries per-device mapping profile state; Apple Magic Keyboard profile detection and `Fn+Esc` mode-toggle tracking are wired as a policy scaffold.
- BTstack PIN/SSP confirmation events are explicitly accepted only while pairing mode is active.
- LE pairing/re-encryption completion now gates BLE HID service client bring-up (`hids_client_connect`) before report routing begins.
- Platform glue records diagnostics in a structured queue (`platform_diag_take`) and mirrors state-change logs to stdio when telemetry is enabled.
- When both `APP_PLATFORM_ENABLE_TELEMETRY` and `APP_PLATFORM_ENABLE_DIAG_CDC` are enabled, diagnostics snapshots are also emitted over TinyUSB CDC as framed binary records (magic/version/payload + monotonic sequence).
- BTstack now persists classic link keys and LE device records through TLV flash-bank storage.
- Pair DB persistence now uses a dual-slot flash journal with sequence-based latest selection and no-op write suppression.
- Main loop now coalesces Pair DB writes with debounce/max-stale windows to reduce flash wear from bursty metadata updates.
- Release guardrails now reject telemetry/diagnostics options in `Release` unless explicitly overridden.
- Factory reset command now erases Pair DB + BTstack persistence sectors and reboots after the LED cue sequence.

## Diagnostics Transport

- Source: app emits `hid_transport_diag_snapshot_t` each tick through `platform_output_t`.
- Queue: when `APP_PLATFORM_ENABLE_TELEMETRY=ON`, platform keeps a bounded diagnostics queue for `platform_diag_take(...)`.
- Host path: when both `APP_PLATFORM_ENABLE_TELEMETRY=ON` and `APP_PLATFORM_ENABLE_DIAG_CDC=ON`, TinyUSB CDC interface `0` publishes each changed snapshot as a framed binary record.
- Host capture helper: `tool/src/diag_capture.c` decodes CDC frames into CSV for offline analysis.
- Host summary helper: `tool/bin/diag_summary` computes soak-level max/delta metrics from captured CSV and can enforce explicit gating thresholds.
- Host alert helper: `tool/bin/diag_alert` renders markdown gate reports for CI/inbox notifications and mirrors gate exit status.
- Framing:
  - `magic`: `0x48 0x52` (`'H' 'R'`)
  - `version`: `1`
  - `payload_len`: `39`
  - payload fields: sequence + bridge queue counters + stack event-queue counters + reconnect counters from `hid_transport_diag_snapshot_t`

## Build/Bootstrap Model

- `make bootstrap APP_PLATFORM=<target>`:
  - downloads local Pico SDK checkout (`.cache/sdk/...`)
  - initializes Pico SDK submodules (TinyUSB, BTstack, etc.)
  - downloads local Arm embedded GCC (`.cache/tool/...`)
  - generates a toolchain file and CMake cache seed in `.cache/`
- `make build APP_PLATFORM=<target>`:
  - runs configure and compile using local cached artifacts

No global Pico SDK or global Arm cross toolchain is required.

## Pair DB Persistence

- Pair DB is serialized into a fixed blob format with magic/version/sequence/checksum.
- Pico W implementation stores Pair DB in two alternating sectors ahead of BTstack storage
  (`PICO_FLASH_SIZE_BYTES - PICO_W_BTSTACK_FLASH_BANK_TOTAL_SIZE - (2 * FLASH_SECTOR_SIZE)`).
- BTstack TLV persistence uses two sectors at the end of flash for link-key and LE device data.
- On boot, `platform_pair_db_load` seeds app state if the stored blob validates.
- On Pair DB mutation, the main loop coalesces writes before `platform_pair_db_save` (2s debounce, 15s max stale, 5s retry backoff).
- Current on-flash schema version is `5`; schema mismatches fall back to an empty DB.
- Legacy schema `4` (dual-slot) and schema `3` (single-slot legacy offset) blobs are accepted on boot and migrated in-memory.
- Factory reset erases both Pair DB sectors and BTstack TLV sectors together, then reboots to clear runtime stack state.

## Resource Cleanup Policy

Project code uses mandatory cleanup attributes.

- Cleanup entry points:
  - `util_cleanup_freep`
  - `util_cleanup_filep`
  - `util_cleanup_fdp`
- Scope macros:
  - `UTIL_SCOPED_FREE`
  - `UTIL_SCOPED_FILE`
  - `UTIL_SCOPED_FD`

The host helper `tool/src/cache_probe.c` demonstrates scoped cleanup with heap buffers, `FILE *`, and file descriptors.

Additional style constraints in this repository:

- C17 only (no C++)
- explicit ownership and small functions
- `calloc` preferred over `malloc` where dynamic allocation is needed

## Planned Bridging Flow (Next Iteration)

1. Tune reconnect retry thresholds/escalation with long-run field telemetry.
2. Extend descriptor remap beyond current boot-profile + keyboard-LED handling into broader host edge-case translation/remapping, including Apple keyboard Fn/media behavior.
3. Add alerting/inbox workflow integration on top of soak diagnostics gate failures.
4. Keep platform glue thin so additional targets can supply equivalent stack hooks.
