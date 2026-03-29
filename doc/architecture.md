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
  - emits reconnect requests and diagnostic snapshots as part of platform output
- `button_fsm`:
  - translates BOOTSEL press patterns into high-level commands (`pair-any`, `remove-last`, `remove-all`)
- `led_ui`:
  - provides user-facing LED state behavior independent from GPIO details
- `pair_db`:
  - paired-device store abstraction with paired timestamp and last-session metadata (descriptor length, protocol mode, vendor/product IDs, reconnect flag)
  - tracks reconnect failure/backoff metadata per device for retry scheduling
  - persisted on Pico W in a flash-backed blob through platform pair-store hooks
- `bt_manager`:
  - Bluetooth management API with pairing lifecycle and active HID session model
  - exposes event-ingest hooks for HID open/close/descriptor/protocol updates
- `usb_bridge`:
  - USB-facing interface plan derived from active BT HID sessions
  - tracks descriptor generation for dynamic USB descriptor rebuild triggers
  - holds bounded routing queues for BT->USB and USB->BT HID reports
  - tracks queue depth/high-water/drop telemetry for backpressure visibility
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
  - `platform_pico_w_stack.*` (optional TinyUSB/BTstack bring-up hooks + USB plan handoff)
  - `platform_pico_w_tinyusb_runtime.*` (TinyUSB runtime calls isolated from BTstack includes)
  - `platform_pico_w_tinyusb_desc.c` (dynamic HID configuration descriptor callbacks)
- platform-local stack config headers:
  - `include/tusb_config.h`
  - `include/btstack_config.h`
- optional Pico SDK stack linkage flags:
  - `APP_PLATFORM_ENABLE_TINYUSB`
  - `APP_PLATFORM_ENABLE_BTSTACK`

Pico-specific linkage is isolated under this directory.

## Current Integration Milestone

- TinyUSB stack can be enabled and built with a baseline HID device configuration.
- BTstack libraries can be enabled and built with project-local `btstack_config.h`.
- Platform runtime initializes optional stacks from `platform_pico_w_stack`.
- Common `bt_manager` now models active HID sessions, not only pair count.
- `usb_bridge` composes a per-interface plan from active sessions and increments descriptor generation on topology changes.
- TinyUSB descriptor callbacks now build a configuration descriptor dynamically (0..8 HID interfaces).
- BTstack HID open/close/report events are now translated into app transport events.
- BTstack HID descriptor/protocol events are now translated into app transport events.
- TinyUSB output report callbacks are now translated into app transport events.
- App and bridge now route queued reports in both directions (one dequeued report per direction per tick) with protocol-aware BT transmission.
- Pair-any mode now drives real BT inquiry/connect attempts under a class-of-device filter and pairing-mode gating.
- App now derives per-interface USB descriptor/protocol hints from active sessions and emits them with each platform output.
- App now emits reconnect requests from persisted Pair DB metadata when idle, with per-device backoff windows.
- Platform stack can consume reconnect requests and invoke BT HID reconnect attempts.
- App reconnect policy now applies per-device backoff windows and timeout-based failure classification.
- Platform stack now emits reconnect result events for immediate reject/connect/auth outcomes.
- App reconnect policy now applies per-result handling (transient stack reject retry, connect-failure backoff, auth-failure disable).
- App reconnect policy now escalates to auto-reconnect disable after repeated connect/timeout failures.
- TinyUSB report descriptor callbacks now export per-interface descriptors directly from BTstack HID descriptor storage when available, with structural sanitization checks.
- BTstack PIN/SSP confirmation events are explicitly accepted only while pairing mode is active.
- Platform glue now records diagnostics in a structured queue (`platform_diag_take`) and mirrors state-change logs to stdio.

## Build/Bootstrap Model

- `make bootstrap`:
  - downloads local Pico SDK checkout (`.cache/sdk/...`)
  - initializes Pico SDK submodules (TinyUSB, BTstack, etc.)
  - downloads local Arm embedded GCC (`.cache/tool/...`)
  - generates a toolchain file and CMake cache seed in `.cache/`
- `make build`:
  - runs configure and compile using local cached artifacts

No global Pico SDK or global Arm cross toolchain is required.

## Pair DB Persistence

- Pair DB is serialized into a fixed blob format with magic/version/checksum.
- Pico W implementation stores this blob in the last flash sector (`PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE`).
- On boot, `platform_pair_db_load` seeds app state if the stored blob validates.
- On Pair DB mutation, the main loop calls `platform_pair_db_save`.
- Current on-flash schema version is `3`; schema mismatches fall back to an empty DB.

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

The host helper `tool/cache_probe.c` demonstrates scoped cleanup with heap buffers, `FILE *`, and file descriptors.

Additional style constraints in this repository:

- C17 only (no C++)
- explicit ownership and small functions
- `calloc` preferred over `malloc` where dynamic allocation is needed

## Planned Bridging Flow (Next Iteration)

1. Expand descriptor translation/sanitization policy beyond current structural checks for host-compatibility edge cases.
2. Persist and restore Bluetooth security/link keys together with Pair DB lifecycle.
3. Expose structured diagnostics queue over a host-visible transport path (USB CDC/vendor endpoint).
4. Tune reconnect retry thresholds/escalation with long-run field telemetry.
5. Keep platform glue thin so additional targets can supply equivalent stack hooks.
