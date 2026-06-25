# Build and Release

## Requirements

Install standard developer tools:

- `make`
- `cmake`
- `git`
- `curl`
- `tar`
- `unzip`
- host C compiler, such as `gcc` or `clang`
- Python 3 for ESP-IDF bootstrap

The repository bootstraps target SDKs and toolchains into `.cache/`; no global Pico SDK, ESP-IDF, BTstack, TinyUSB, or Arm cross-toolchain install is required.

## Common Targets

```sh
make help
make platform-list
make build APP_PLATFORM=<target>
make test-host
make release-<target>
make release
```

Build trees are written under `build/`. Downloaded SDK/toolchain material is written under `.cache/`.

## Release Artifacts

Release builds are the files meant to publish on GitHub Releases. They use the same platform build system as developer builds, but with debug-only behavior turned off.

To build one platform:

```sh
make release-pico_w
```

To build everything currently present under `platform/`:

```sh
make release
```

The aggregate target discovers platform targets under `platform/`, dispatches to each matching `release-<target>`, and writes publishable artifacts to `build/release/`.

Production release settings are intentionally plain:

- TinyUSB and BTstack are enabled.
- Telemetry is disabled.
- Diagnostics CDC is disabled.
- Wipe-on-boot is disabled.

Exact CMake settings:

```sh
-DCMAKE_BUILD_TYPE=Release
-DAPP_PLATFORM_ENABLE_TINYUSB:BOOL=ON
-DAPP_PLATFORM_ENABLE_BTSTACK:BOOL=ON
-DAPP_PLATFORM_ENABLE_TELEMETRY:BOOL=OFF
-DAPP_PLATFORM_ENABLE_DIAG_CDC:BOOL=OFF
-DAPP_PLATFORM_DEBUG_WIPE_ALL_ON_BOOT:BOOL=OFF
```

Current release matrix:

| Board | Configure parameters | Build tree | Release artifact |
| --- | --- | --- | --- |
| Raspberry Pi Pico W | `-DAPP_PLATFORM=pico_w -DPICO_BOARD=pico_w` | `build/release-build/pico_w/` | `build/release/hidrelay-fw-pico-w.uf2` |
| Raspberry Pi Pico 2 W | `-DAPP_PLATFORM=pico_2_w -DPICO_BOARD=pico2_w` | `build/release-build/pico_2_w/` | `build/release/hidrelay-fw-pico-2-w.uf2` |

## Flashing Release Builds

Use the smallest practical flashing path for each board.

### Raspberry Pi Pico W / Pico 2 W

1. Build or download the Pico W / Pico 2 W UF2.
2. Hold BOOTSEL while plugging the board into USB.
3. Copy the matching `hidrelay-fw-pico-w.uf2` or `hidrelay-fw-pico-2-w.uf2` to the `RPI-RP2` drive.

That's it!

## Developer Builds

Build the default firmware for one target:

```sh
make build APP_PLATFORM=pico_w
```

Show available targets:

```sh
make platform-list
```

Run host-side deterministic validation without hardware:

```sh
make test-host
```

Debug builds enable telemetry and diagnostics by default where supported:

```sh
cmake -S . -B build/pico_w_debug \
  -DAPP_PLATFORM=pico_w \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAPP_PLATFORM_ENABLE_TELEMETRY=ON \
  -DAPP_PLATFORM_ENABLE_DIAG_CDC=ON
cmake --build build/pico_w_debug --parallel
```

Wipe persisted pairing/security state on every boot for debug-only testing:

```sh
make build APP_PLATFORM=pico_w APP_DEBUG_WIPE_ALL_ON_BOOT=ON
```

## Diagnostics Tools

Host-side diagnostics helpers are available for debug/soak workflows:

```sh
make tool-diag-capture
make tool-diag-summary INPUT=diag.csv
make tool-diag-gate INPUT=diag.csv
make tool-diag-alert INPUT=diag.csv OUTPUT=diag_report.md
```

See [soak.md](soak.md) for the long-run diagnostics workflow.
