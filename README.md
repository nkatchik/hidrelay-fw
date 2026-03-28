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
- BT manager active-HID session model with BTstack event ingestion (`bt_manager_ingest_hid_open/close`)
- USB bridge interface-plan model with descriptor generation tracking and bounded report queues
- TinyUSB configuration descriptor composition for 0..8 HID interfaces
- bidirectional report path skeleton (`BT HID report` -> USB IN, `USB OUT report` -> BT HID set-report)
- flash-backed pair database persistence (last sector of on-board flash)
- BOOTSEL button command FSM for:
  - pair-any
  - remove-last
  - remove-all

## Quick Start

```sh
make bootstrap
make build
```

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

These options remain off by default for fast baseline iteration, but the repository now includes working starter configs in:

- `platform/pico_w/include/tusb_config.h`
- `platform/pico_w/include/btstack_config.h`

When stack options are enabled:

- `platform_pico_w_stack` initializes BTstack and TinyUSB
- BTstack HID open/close/report events are bridged into common app transport events
- TinyUSB `set_report` callbacks are bridged into common app transport events
- TinyUSB descriptor callbacks build configuration descriptors from the current interface plan
- one queued report per tick is forwarded in each direction via `usb_bridge`

## Repository Layout

- `cmake/` - bootstrap and build modules
- `doc/` - architecture and build documentation
- `include/` - public headers for app/platform modules
- `src/` - platform-agnostic firmware logic
- `platform/` - platform-specific glue (`platform/pico_w/` now)
- `tool/` - host-side helper code (`cache_probe` cleanup demo)

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

## Coding Rules

Project-owned C code uses `__attribute__((cleanup(...)))` for resource disposal.

- No explicit `free`, `fclose`, or `close` outside cleanup helper internals.
- Cleanup helpers are defined in `include/util/cleanup.h` and `src/util/cleanup.c`.

## Next Work

See:

- `doc/architecture.md`
- `doc/build.md`

Next implementation steps:

- move pair-any from pairing-state-only behavior to real BT discovery/connect flow and security policy
- switch from generic HID report templates to per-device descriptor/protocol handling
- improve queue/backpressure behavior and telemetry under high report rates
- persist active-session metadata needed for reconnect policy across reboots
