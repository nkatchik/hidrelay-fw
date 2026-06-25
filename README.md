# hidrelay-fw

Bluetooth-to-USB HID relay firmware. The device pairs with Bluetooth keyboards, mice, and trackpads, then exposes them to the USB host as HID devices.

If this project is useful to your HID, accessibility, or embedded work, please star the repository.
Public adoption helps the project.

## Architecture

- Bare-metal firmware: no Linux kernel, userspace services, init system, or device tree in the runtime path.
- Bluetooth and USB are driven directly from the firmware event loop through platform-owned stack glue.
- Avoiding Linux keeps boot time, latency, storage needs, and background scheduling noise low for HID relay use.
- The smaller runtime surface also reduces field-update complexity and leaves more flash/RAM for device state, report policy, and diagnostics.
- Shared app logic lives in `src/` and `include/`; board/SDK details stay under `platform/`.

## Installation

Release files are production builds: debug telemetry and diagnostic USB serial are off, and the firmware will not wipe pairing data on boot.

Pico-family builds are for the Bluetooth-capable W boards. Plain Pico/Pico 2 boards do not include the radio this firmware needs.

### Raspberry Pi Pico W / Pico 2 W

1. Download [⬇️ Pico W firmware](/releases/latest/download/hidrelay-fw-pico-w.uf2) / [⬇️ Pico 2 W firmware](/releases/latest/download/hidrelay-fw-pico-2-w.uf2).
2. Hold BOOTSEL while plugging the board into USB.
3. Wait for the `RPI-RP2` drive to appear on Pico W, or `RP2350` on Pico 2 W.
4. Copy the downloaded UF2 onto that drive. The board reboots into the firmware.

That's it!

## Usage

The BOOTSEL/user button controls pairing and reset flows:

- Hold 1 second: enter BLE pairing mode.
- Keep holding to 5 seconds: switch to Classic pairing mode.
- Hold 10 seconds: factory reset pairing and Bluetooth security state.

BLE and Classic pairing modes are intentionally exclusive.

## Build

Requirements: `make`, `cmake`, `git`, `curl`, `tar`, `unzip`, a C compiler for host tools, and Python 3 for ESP-IDF bootstrap.

Build one firmware target:

```sh
make bootstrap APP_PLATFORM=pico_w
make build APP_PLATFORM=pico_w
```

Build one release artifact or all platform releases:

```sh
make release-pico_w
make release
```

Release outputs are written to `build/release/`.

Run host-side deterministic tests:

```sh
make test-host
```

## Repository Layout

- `src/` and `include/` - shared firmware logic.
- `platform/` - hardware, SDK, Bluetooth-controller, USB-device, flash, and flashing glue.
- `tool/` - host-side replay and diagnostics utilities.
- `doc/` - build, architecture, and soak-test notes.

## Documentation

- [Architecture](doc/architecture.md)
- [Build and release](doc/build.md)
- [Soak testing](doc/soak.md)

## License

See [LICENSE](LICENSE).
