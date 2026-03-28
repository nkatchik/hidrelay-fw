# hidrelay-fw

Bare-metal firmware skeleton for an open source Bluetooth HID to USB HID hub.

Default target: **Raspberry Pi Pico W**.

This repository is intentionally conservative: it focuses on build/bootstrap structure and clean module boundaries, while Bluetooth and USB bridge logic are currently stubs.

## Status

Current implementation is a buildable skeleton with:

- local toolchain and SDK bootstrap
- platform-selectable CMake layout (auto-discovers from `platform/<name>/`)
- app event loop and module boundaries for future HID bridging
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

These options are intentionally off by default in this first pass because full stack config headers (`tusb_config.h`, `btstack_config.h`) are part of the next implementation phase.

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

Planned next step is replacing stubs with real BTstack HID host + TinyUSB HID device bridging while keeping module boundaries intact.
