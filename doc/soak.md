# Soak Test Runbook

## Purpose

Validate long-run bridge stability and queue behavior with diagnostics capture and trend checks.

## Prerequisites

- Pico W flashed with a debug/development build that enables telemetry + diagnostics CDC.
- Host can access the Pico CDC port (for example `/dev/tty.usbmodemXXXX`).
- `make tool-diag-capture` and `tool/diag_summary.sh` available in this repository.

## Build

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

Flash `build/pico_w_debug/platform/pico_w/hidrelay_fw.uf2` to the board.

## Capture

Build host capture tool:

```sh
make tool-diag-capture
```

Capture diagnostics for a target window (example: 8 hours):

```sh
build/tool/diag_capture \
  --device /dev/tty.usbmodemXXXX \
  --baud 115200 \
  --output soak.csv
```

Optional bounded sample capture:

```sh
build/tool/diag_capture \
  --device /dev/tty.usbmodemXXXX \
  --baud 115200 \
  --count 10000 \
  --output soak.csv
```

## Summarize

```sh
make tool-diag-summary INPUT=soak.csv
```

This prints:

- max queue depths/high-water marks
- reconnect counter deltas
- drop counter deltas (`usb_tx`, `bt_tx`, `stack_event`)
- a simple result line (`ok_no_drops` or `warning_drops_detected`)

## Operational Checks

- `usb_tx_dropped_delta == 0`
- `bt_tx_dropped_delta == 0`
- `stack_event_dropped_delta == 0`
- reconnect failures do not trend upward without corresponding successful recovery
- queue depth/high-water metrics remain bounded under expected workload

## If Drops Occur

1. Keep the same firmware image and reproduce with a shorter scenario to isolate trigger conditions.
2. Correlate rising drop counters with reconnect events and active device count in the CSV timeline.
3. Adjust reconnect pacing/queue policy and rerun the same soak profile for comparison.
