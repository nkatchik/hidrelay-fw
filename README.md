# hidrelay-fw

Bluetooth-to-USB HID relay firmware.

The device pairs with Bluetooth keyboards, mice, and trackpads, then exposes them to the USB host as HID devices.

If this project is useful to your HID, accessibility, or embedded work, please star the repository.
Public adoption helps the project.

## Basics

- Bare-metal firmware: no Linux kernel, userspace services, init system, or device tree in the runtime path.
- Bluetooth and USB are driven directly from the firmware event loop.
- Independent support for Apple Magic accessories (Classic pairing only).

## Installation

### Raspberry Pi Pico W / Pico 2 W

1. Download [⬇ Pico W firmware](https://github.com/nkatchik/hidrelay-fw/releases/latest/download/hidrelay-fw-pico-w.uf2) / [⬇ Pico 2 W firmware](https://github.com/nkatchik/hidrelay-fw/releases/latest/download/hidrelay-fw-pico-2-w.uf2).
2. Hold `BOOTSEL` button while plugging the board into USB.
3. Wait for the `RPI-RP2` / `RP2350` drive to appear.
4. Drag the downloaded UF2 onto that drive.

The board will reboot into the firmware. That's it!

## Usage

The `BOOTSEL` button controls pairing and reset flows:

- Hold **1 second**: enter BLE pairing mode.
- Hold **3 seconds**: switch to Classic pairing mode.
- Hold **8 seconds**: factory reset pairing and Bluetooth security state.

BLE and Classic pairing modes are intentionally exclusive.

### Apple Magic Keyboard

Classic pairing mode only.

macOS's media/function-key checkbox is not respected through this firmware.
Use `Fn` + `Esc` to toggle the relay-side top-row mode.

| Shortcut | USB host key |
| --- | --- |
| `Fn` + `Esc` | Toggle top row between media keys and F1-F12 |
| `Eject ⏏` | Lock Screen |
| `Media 5` | Voice Assistant |
| `Media 6` | Do Not Disturb |

### Apple Magic Trackpad

Classic pairing mode only.

Magic Trackpad gestures are not passed through to the USB host as native
gestures. The firmware interprets trackpad input and does best-effort emulation
using ordinary USB mouse and keyboard output.

Gesture emulation expects these shortcuts to be enabled or available on macOS:

| Trackpad input | Firmware emits | Expected macOS action |
| --- | --- | --- |
| Three-finger swipe up | `Control` + `Up Arrow` | Mission Control |
| Three-finger swipe down | `Control` + `Down Arrow` | Application windows / App Expose |
| Three-finger swipe left | `Control` + `Right Arrow` | Move right a space |
| Three-finger swipe right | `Control` + `Left Arrow` | Move left a space |
| Pinch out | `Command` + `=` | Zoom In in the active app |
| Pinch in | `Command` + `-` | Zoom Out in the active app |

## Documentation

- [Architecture](doc/architecture.md)
- [Build and release](doc/build.md)
- [Soak testing](doc/soak.md)

## License

See [LICENSE](LICENSE).
