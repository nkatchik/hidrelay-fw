# Basic

This is microcontroller firmware that turns the device into a Bluetooth-to-HID relay.

Once flashed, the device acts as a Bluetooth host for Bluetooth devices and as a USB HID hub for the USB host.

# Rules

- The app has two separate pairing modes: BLE and Classic.
  - These modes are intentionally exclusive.
  - They may share code when it makes sense for the relevant logic.
- All platform-dependent code must be located in the `platform` directory.
- Platform names must not be mentioned outside the `platform` directory, except in documentation files.
- Prioritize keeping app and Bluetooth logic outside the `platform` directory unless the logic is platform-specific.

# Skills

## Overview

This guide covers firmware development workflows including debug builds, device flashing, diagnostic capture/analysis, and troubleshooting using the built-in diagnostic infrastructure.

## Debug Builds

### Standard Debug Build (Telemetry + Diagnostics)

Create a debug build with telemetry and CDC diagnostics enabled:

```sh
make configure APP_PLATFORM=pico_w CMAKE_BUILD_TYPE=Debug
make build APP_PLATFORM=pico_w
```

Or directly with the build task (using `🛠️🐞 Build` from VS Code tasks).

This enables:
- `APP_PLATFORM_ENABLE_TELEMETRY=ON` - structured diagnostics snapshots
- `APP_PLATFORM_ENABLE_DIAG_CDC=ON` - TinyUSB CDC diagnostics transport
- Verbose logging of state changes, reconnects, and queue metrics

### Custom Debug Configuration

Build with specific stack toggles:

```sh
cmake -S . -B build/pico_w_debug \
  -DAPP_PLATFORM=pico_w \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAPP_PLATFORM_ENABLE_TINYUSB=ON \
  -DAPP_PLATFORM_ENABLE_BTSTACK=ON \
  -DAPP_PLATFORM_ENABLE_TELEMETRY=ON \
  -DAPP_PLATFORM_ENABLE_DIAG_CDC=ON
cmake --build build/pico_w_debug --parallel
```

### Disable Telemetry (Production-like Debug)

If you need to debug without telemetry overhead:

```sh
cmake -S . -B build/pico_w_debug_no_telemetry \
  -DAPP_PLATFORM=pico_w \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAPP_PLATFORM_ENABLE_TELEMETRY=OFF \
  -DAPP_PLATFORM_ENABLE_DIAG_CDC=OFF
cmake --build build/pico_w_debug_no_telemetry --parallel
```

### Debug Build with Wipe-on-Boot

For testing clean state behavior, enable the debug wipe flag:

```sh
make build APP_PLATFORM=pico_w APP_DEBUG_WIPE_ALL_ON_BOOT=1
```

This erases pair database and BTstack security material on each boot, useful for pairing mode testing.

## Flashing

### Standard Flash Workflow

1. **Build the firmware:**
   ```sh
   make build APP_PLATFORM=pico_w
   ```

2. **Flash to device:**
   ```sh
   platform/pico_w/bin/picotool reboot -f -u
   ```

   Or use the VS Code `⚡ Flash` task.

The `.uf2` artifact is typically at:
- `build/pico_w/platform/pico_w/hidrelay_fw.uf2`

### Force BOOTSEL Without Button

Trigger BOOTSEL mode remotely:

```sh
platform/pico_w/bin/picotool reboot -f -u
```

This reboots into BOOTSEL (USB mass storage) mode without requiring physical button press.

### Manual Flash (BOOTSEL Mode)

1. Hold BOOTSEL button and plug USB (or press BOOTSEL + Reset)
2. Device appears as RPI-RP2 disk
3. Copy `.uf2` file to the disk:
   ```sh
   cp build/pico_w/platform/pico_w/hidrelay_fw.uf2 /Volumes/RPI-RP2/
   ```

### Verify Flash

Check serial output after flashing (requires debug telemetry build):

```sh
# Find the serial port
ls /dev/tty.usbmodem*

# Connect to serial output
screen /dev/tty.usbmodem<number> 115200
```

Press Ctrl-A, then D to exit screen.

## Diagnostic Capture

### Prerequisites

- Device flashed with a debug build (telemetry + CDC enabled)
- USB cable connected to Pico W
- Host tools built

### Build Host Tools

```sh
make tool-diag-capture
```

This builds the `build/tool/diag_capture` utility.

### Capture Diagnostics

Capture unlimited diagnostics to CSV:

```sh
build/tool/diag_capture \
  --device /dev/tty.usbmodem<number> \
  --baud 115200 \
  --output diagnostics.csv
```

Capture bounded sample (e.g., 10,000 snapshots):

```sh
build/tool/diag_capture \
  --device /dev/tty.usbmodem<number> \
  --baud 115200 \
  --count 10000 \
  --output diagnostics.csv
```

Press Ctrl-C to stop live capture.

### Capture Duration Estimation

Each snapshot is typically 1-5 ms apart depending on activity. Rough estimates:

- 10,000 snapshots ≈ 10-50 seconds
- 100,000 snapshots ≈ 2-5 minutes
- 1,000,000 snapshots ≈ 15-60 minutes

## Diagnostic Analysis

### Summarize Diagnostics

Print key metrics and high-water marks:

```sh
make tool-diag-summary INPUT=diagnostics.csv
```

Output includes:
- Queue depth and high-water marks
- Reconnect counter deltas
- Drop counters (USB TX, BT TX, stack event)
- Result status (`ok_no_drops` or `warning_drops_detected`)

### Gate on Thresholds

Enforce strict no-drop threshold:

```sh
make tool-diag-gate INPUT=diagnostics.csv
```

Gate with reconnect failure limit:

```sh
make tool-diag-gate INPUT=diagnostics.csv MAX_RECONNECT_FAILURE_DELTA=0
```

Exit codes:
- `0`: all thresholds passed
- `3`: one or more thresholds failed
- `2`: invalid input/arguments

### Generate Alert Report

Create a markdown report for notifications/alerts:

```sh
make tool-diag-alert INPUT=diagnostics.csv OUTPUT=report.md
```

With reconnect failure gating:

```sh
make tool-diag-alert INPUT=diagnostics.csv OUTPUT=report.md MAX_RECONNECT_FAILURE_DELTA=0
```

Report includes:
- Key metrics summary
- Gate pass/fail rows
- Recommended actions for failures

Exit code mirrors gate status.

## Soak Test Workflow

Complete workflow for multi-hour stability testing:

### 1. Build Debug Firmware

```sh
cmake -S . -B build/pico_w_debug \
  -DAPP_PLATFORM=pico_w \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAPP_PLATFORM_ENABLE_TINYUSB=ON \
  -DAPP_PLATFORM_ENABLE_BTSTACK=ON \
  -DAPP_PLATFORM_ENABLE_TELEMETRY=ON \
  -DAPP_PLATFORM_ENABLE_DIAG_CDC=ON
cmake --build build/pico_w_debug --parallel
```

### 2. Flash Device

```sh
platform/pico_w/bin/picotool reboot -f -u
cp build/pico_w_debug/platform/pico_w/hidrelay_fw.uf2 /Volumes/RPI-RP2/
```

### 3. Prepare Test Environment

Pair Bluetooth devices with the hub and set up automated interaction (button presses, device switching, etc.).

### 4. Capture Diagnostics

```sh
make tool-diag-capture
build/tool/diag_capture \
  --device /dev/tty.usbmodem<number> \
  --baud 115200 \
  --output soak.csv
```

Let capture run for the desired test window (e.g., 8 hours). Monitor the output file size.

### 5. Analyze Results

```sh
make tool-diag-summary INPUT=soak.csv
```

Check for:
- `usb_tx_dropped_delta == 0`
- `bt_tx_dropped_delta == 0`
- `stack_event_dropped_delta == 0`
- Queue depths remain bounded

### 6. Generate Report

```sh
make tool-diag-alert INPUT=soak.csv OUTPUT=soak_report.md MAX_RECONNECT_FAILURE_DELTA=0
```

## Troubleshooting

### Device Not Detected

**Issue:** `error: unknown device 'xxxxxxxx'` when using picotool.

**Solution:**
1. Check USB connection
2. Manually enter BOOTSEL mode (hold button + plug USB)
3. Verify device appears in `ls /Volumes/RPI-RP2/` (macOS) or `lsblk` (Linux)

### Capture Tool Not Finding Device

**Issue:** `diag_capture` fails to connect to serial device.

**Solution:**
1. Find correct port: `ls /dev/tty.usbmodem*`
2. Verify baud rate matches (default 115200)
3. Ensure firmware has diagnostics CDC enabled (`APP_PLATFORM_ENABLE_DIAG_CDC=ON`)

### High Drop Counts in Diagnostics

**Issue:** Soak test shows `usb_tx_dropped` or `bt_tx_dropped > 0`.

**Diagnosis:**
1. Check queue high-water marks vs configured limits
2. Correlate drops with reconnect events in CSV timeline
3. Note active device count during drop events

**Mitigations:**
1. Reduce reconnect pacing (increase backoff windows)
2. Increase queue size limits (if configurable)
3. Reduce active device count / throughput
4. Test with simpler device combinations

### Intermittent Reconnect Failures

**Issue:** Reconnects fail sporadically during soak.

**Diagnosis:**
1. Capture with `MAX_RECONNECT_FAILURE_DELTA` gate on soak
2. Check Bluetooth link quality (RSSI, interference)
3. Note device types involved (phone, keyboard, mouse, etc.)

**Mitigations:**
1. Improve antenna placement / reduce RF interference
2. Test with fewer simultaneous devices
3. Check device Bluetooth firmware versions
4. Validate reconnect transport hints in Pair DB (LE vs Classic, address type)

### CDC Port Not Appearing

**Issue:** No `/dev/tty.usbmodem*` device after flashing debug build.

**Diagnosis:**
1. Verify `APP_PLATFORM_ENABLE_DIAG_CDC=ON` in build
2. Check that `APP_PLATFORM_ENABLE_TINYUSB=ON`
3. Confirm device re-enumerated after flash
4. Try unplugging and replugging USB

**Workaround:**
- Build without CDC (CDC is optional for debugging, stdio logs still available)
- Try a different USB cable or port

## Diagnostic CSV Format

The diagnostic snapshots are captured as framed binary records and exported as CSV with columns:

- `timestamp` - monotonic sequence counter
- `queue_depth_usb_rx` - USB report queue depth
- `queue_depth_bt_tx` - Bluetooth TX queue depth
- `queue_depth_stack_event` - Stack event queue depth
- `queue_high_water_usb_rx` - Peak USB RX queue depth since last capture
- `queue_high_water_bt_tx` - Peak BT TX queue depth since last capture
- `queue_high_water_stack_event` - Peak stack event queue depth since last capture
- `usb_tx_dropped` - USB TX dropped report count
- `bt_tx_dropped` - BT TX dropped report count
- `stack_event_dropped` - Stack event dropped count
- `reconnect_count` - Total reconnect attempts
- Other device/state metrics (varies by build)

## Build Artifacts

After `make build APP_PLATFORM=pico_w`, key outputs appear in `build/pico_w/platform/pico_w/`:

- `hidrelay_fw.elf` - ELF binary (debug symbols, disassembly)
- `hidrelay_fw.bin` - Raw binary image
- `hidrelay_fw.uf2` - UF2 format (for BOOTSEL flashing)
- `hidrelay_fw.map` - Linker map file (symbol addresses)
- `hidrelay_fw.dis` - Disassembly listing

## Cleaning Build Artifacts

Remove build tree for a specific platform:

```sh
make clean APP_PLATFORM=pico_w
```

Remove all build artifacts:

```sh
make clean
```

Full clean including cached SDK/toolchain:

```sh
make distclean
```

</skill>
