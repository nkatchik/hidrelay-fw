# Soak Test Runbook

## Purpose

Validate long-run bridge stability and queue behavior with diagnostics capture, trend checks, and threshold gates.

## Prerequisites

- Pico W flashed with a debug/development build that enables telemetry + diagnostics CDC.
- Host can access the Pico CDC port (for example `/dev/tty.usbmodemXXXX`).
- `make tool-diag-capture` and `tool/bin/diag_summary` available in this repository.

## Build

```sh
cmake -S . -B build/pico_w_debug \
  -DAPP_PLATFORM=pico_w \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAPP_PLATFORM_ENABLE_TINYUSB=ON \
  -DAPP_PLATFORM_ENABLE_BTSTACK=ON \
  -DAPP_PLATFORM_ENABLE_TELEMETRY=ON \
  -DAPP_PLATFORM_ENABLE_DIAG_CDC=ON \
  -DAPP_PLATFORM_OPERATOR_AUTH_KEY_HEX=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff
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

## Gate

Enforce strict no-drop gate:

```sh
make tool-diag-gate INPUT=soak.csv
```

Optionally gate reconnect failures too:

```sh
make tool-diag-gate INPUT=soak.csv MAX_RECONNECT_FAILURE_DELTA=0
```

Gate behavior:

- `make tool-diag-gate ...` returns non-zero on any gate failure.
- Direct script exit codes:
  - exit `0`: all configured thresholds passed
  - exit `3`: at least one configured threshold failed
  - exit `2`: invalid input/arguments

## Alert Report

Generate a markdown report suitable for inbox/notification pipelines:

```sh
make tool-diag-alert INPUT=soak.csv OUTPUT=soak_report.md MAX_RECONNECT_FAILURE_DELTA=0
```

Behavior:

- report output includes key metrics, gate rows, and recommended next actions on failure
- command exit code mirrors gate status (`0` pass, `3` fail, `2` invalid usage/input)

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
