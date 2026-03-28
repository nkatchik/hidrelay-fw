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

They are disabled by default so this skeleton builds without requiring final TinyUSB/BTstack config headers. Enable them as bridge implementation work starts.

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

Still pending for production behavior:

- BTstack HID host implementation
- TinyUSB dynamic multi-interface HID device composition
- persistent on-flash pair database
- command UX refinement and full failure-recovery handling
