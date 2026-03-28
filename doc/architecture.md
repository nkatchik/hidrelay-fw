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
- `button_fsm`:
  - translates BOOTSEL press patterns into high-level commands (`pair-any`, `remove-last`, `remove-all`)
- `led_ui`:
  - provides user-facing LED state behavior independent from GPIO details
- `pair_db`:
  - in-memory paired-device store abstraction, including per-device paired timestamp metadata
- `bt_manager`:
  - Bluetooth management API with pairing lifecycle and active HID session model
  - exposes event-ingest hooks for HID open/close (`bt_manager_ingest_hid_open/close`)
- `usb_bridge`:
  - USB-facing interface plan derived from active BT HID sessions
  - tracks descriptor generation for dynamic USB descriptor rebuild triggers
- `platform_api`:
  - platform boundary: init, poll inputs, apply outputs

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
- HID transport/data-path is still stubbed (no report forwarding yet).

## Build/Bootstrap Model

- `make bootstrap`:
  - downloads local Pico SDK checkout (`.cache/sdk/...`)
  - initializes Pico SDK submodules (TinyUSB, BTstack, etc.)
  - downloads local Arm embedded GCC (`.cache/tool/...`)
  - generates a toolchain file and CMake cache seed in `.cache/`
- `make build`:
  - runs configure and compile using local cached artifacts

No global Pico SDK or global Arm cross toolchain is required.

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

1. Feed real BTstack HID open/close metadata into `bt_manager_ingest_hid_open/close`.
2. Add HID report routing path from BTstack device channels to TinyUSB HID endpoints and back.
3. Persist Pair DB entries and metadata to flash-backed storage.
4. Add reconnect policy and interface lifecycle robustness across disconnect storms.
5. Keep platform glue thin so additional targets can supply equivalent stack hooks.
