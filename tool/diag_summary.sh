#!/bin/sh
set -eu

usage() {
    printf '%s\n' \
        "Usage: $0 --input <diag.csv> [options]" \
        "" \
        "Options:" \
        "  --require-no-drops                 Enforce zero drop deltas for USB/BT/stack event queues" \
        "  --max-usb-drop-delta <n>           Maximum allowed usb_tx_dropped_delta" \
        "  --max-bt-drop-delta <n>            Maximum allowed bt_tx_dropped_delta" \
        "  --max-stack-event-drop-delta <n>   Maximum allowed stack_event_dropped_delta" \
        "  --max-reconnect-failure-delta <n>  Maximum allowed reconnect_failure_delta" \
        "  -h, --help                         Show this help text"
}

parse_non_negative_int() {
    value="$1"

    case "$value" in
        ''|*[!0-9]*)
            return 1
            ;;
        *)
            return 0
            ;;
    esac
}

input_path=""
max_usb_drop_delta="-1"
max_bt_drop_delta="-1"
max_stack_event_drop_delta="-1"
max_reconnect_failure_delta="-1"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --input)
            shift
            if [ "$#" -eq 0 ]; then
                printf '%s\n' "Missing value for --input" >&2
                usage >&2
                exit 2
            fi
            input_path="$1"
            ;;
        --require-no-drops)
            max_usb_drop_delta="0"
            max_bt_drop_delta="0"
            max_stack_event_drop_delta="0"
            ;;
        --max-usb-drop-delta)
            shift
            if [ "$#" -eq 0 ] || ! parse_non_negative_int "$1"; then
                printf '%s\n' "Invalid value for --max-usb-drop-delta" >&2
                usage >&2
                exit 2
            fi
            max_usb_drop_delta="$1"
            ;;
        --max-bt-drop-delta)
            shift
            if [ "$#" -eq 0 ] || ! parse_non_negative_int "$1"; then
                printf '%s\n' "Invalid value for --max-bt-drop-delta" >&2
                usage >&2
                exit 2
            fi
            max_bt_drop_delta="$1"
            ;;
        --max-stack-event-drop-delta)
            shift
            if [ "$#" -eq 0 ] || ! parse_non_negative_int "$1"; then
                printf '%s\n' "Invalid value for --max-stack-event-drop-delta" >&2
                usage >&2
                exit 2
            fi
            max_stack_event_drop_delta="$1"
            ;;
        --max-reconnect-failure-delta)
            shift
            if [ "$#" -eq 0 ] || ! parse_non_negative_int "$1"; then
                printf '%s\n' "Invalid value for --max-reconnect-failure-delta" >&2
                usage >&2
                exit 2
            fi
            max_reconnect_failure_delta="$1"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'Unknown argument: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if [ -z "$input_path" ]; then
    usage >&2
    exit 2
fi

if [ ! -f "$input_path" ]; then
    printf 'Input file not found: %s\n' "$input_path" >&2
    exit 2
fi

awk -F, \
    -v max_usb_drop_delta="$max_usb_drop_delta" \
    -v max_bt_drop_delta="$max_bt_drop_delta" \
    -v max_stack_event_drop_delta="$max_stack_event_drop_delta" \
    -v max_reconnect_failure_delta="$max_reconnect_failure_delta" \
'
function gate_check(metric_name, actual, limit,    status) {
    if (limit < 0) {
        return
    }

    gate_enabled = 1
    status = (actual <= limit) ? "pass" : "fail"
    printf "gate_%s,actual=%.0f,limit=%.0f,status=%s\n", metric_name, actual, limit, status

    if (status == "fail") {
        gate_failed = 1
    }
}

NR == 1 {
    for (i = 1; i <= NF; i++) {
        col[$i] = i
    }

    required[1] = "host_ms"
    required[2] = "sequence"
    required[3] = "usb_tx_depth"
    required[4] = "bt_tx_depth"
    required[5] = "usb_tx_high_watermark"
    required[6] = "bt_tx_high_watermark"
    required[7] = "usb_tx_dropped"
    required[8] = "bt_tx_dropped"
    required[9] = "reconnect_attempt_count"
    required[10] = "reconnect_success_count"
    required[11] = "reconnect_failure_count"
    required[12] = "stack_event_depth"
    required[13] = "stack_event_high_watermark"
    required[14] = "stack_event_dropped"

    for (i = 1; i <= 14; i++) {
        if (!(required[i] in col)) {
            printf "Missing CSV column: %s\n", required[i] > "/dev/stderr"
            exit 2
        }
    }

    next
}

NR > 1 {
    sample_count++

    host_ms = $(col["host_ms"]) + 0
    sequence = $(col["sequence"]) + 0
    usb_tx_depth = $(col["usb_tx_depth"]) + 0
    bt_tx_depth = $(col["bt_tx_depth"]) + 0
    usb_tx_high_watermark = $(col["usb_tx_high_watermark"]) + 0
    bt_tx_high_watermark = $(col["bt_tx_high_watermark"]) + 0
    usb_tx_dropped = $(col["usb_tx_dropped"]) + 0
    bt_tx_dropped = $(col["bt_tx_dropped"]) + 0
    reconnect_attempt_count = $(col["reconnect_attempt_count"]) + 0
    reconnect_success_count = $(col["reconnect_success_count"]) + 0
    reconnect_failure_count = $(col["reconnect_failure_count"]) + 0
    stack_event_depth = $(col["stack_event_depth"]) + 0
    stack_event_high_watermark = $(col["stack_event_high_watermark"]) + 0
    stack_event_dropped = $(col["stack_event_dropped"]) + 0

    if (sample_count == 1) {
        first_host_ms = host_ms
        first_sequence = sequence
        first_usb_tx_dropped = usb_tx_dropped
        first_bt_tx_dropped = bt_tx_dropped
        first_reconnect_attempt_count = reconnect_attempt_count
        first_reconnect_success_count = reconnect_success_count
        first_reconnect_failure_count = reconnect_failure_count
        first_stack_event_dropped = stack_event_dropped
    }

    last_host_ms = host_ms
    last_sequence = sequence
    last_usb_tx_dropped = usb_tx_dropped
    last_bt_tx_dropped = bt_tx_dropped
    last_reconnect_attempt_count = reconnect_attempt_count
    last_reconnect_success_count = reconnect_success_count
    last_reconnect_failure_count = reconnect_failure_count
    last_stack_event_dropped = stack_event_dropped

    if ((sample_count == 1) || (usb_tx_depth > max_usb_tx_depth)) {
        max_usb_tx_depth = usb_tx_depth
    }
    if ((sample_count == 1) || (bt_tx_depth > max_bt_tx_depth)) {
        max_bt_tx_depth = bt_tx_depth
    }
    if ((sample_count == 1) || (usb_tx_high_watermark > max_usb_tx_high_watermark)) {
        max_usb_tx_high_watermark = usb_tx_high_watermark
    }
    if ((sample_count == 1) || (bt_tx_high_watermark > max_bt_tx_high_watermark)) {
        max_bt_tx_high_watermark = bt_tx_high_watermark
    }
    if ((sample_count == 1) || (stack_event_depth > max_stack_event_depth)) {
        max_stack_event_depth = stack_event_depth
    }
    if ((sample_count == 1) || (stack_event_high_watermark > max_stack_event_high_watermark)) {
        max_stack_event_high_watermark = stack_event_high_watermark
    }
}

END {
    if (sample_count == 0) {
        printf "No diagnostics rows found in input CSV\n" > "/dev/stderr"
        exit 2
    }

    duration_ms = last_host_ms - first_host_ms
    if (duration_ms < 0) {
        duration_ms = 0
    }

    usb_drop_delta = last_usb_tx_dropped - first_usb_tx_dropped
    bt_drop_delta = last_bt_tx_dropped - first_bt_tx_dropped
    reconnect_attempt_delta = last_reconnect_attempt_count - first_reconnect_attempt_count
    reconnect_success_delta = last_reconnect_success_count - first_reconnect_success_count
    reconnect_failure_delta = last_reconnect_failure_count - first_reconnect_failure_count
    stack_event_drop_delta = last_stack_event_dropped - first_stack_event_dropped

    if (usb_drop_delta < 0) {
        usb_drop_delta = 0
    }
    if (bt_drop_delta < 0) {
        bt_drop_delta = 0
    }
    if (reconnect_attempt_delta < 0) {
        reconnect_attempt_delta = 0
    }
    if (reconnect_success_delta < 0) {
        reconnect_success_delta = 0
    }
    if (reconnect_failure_delta < 0) {
        reconnect_failure_delta = 0
    }
    if (stack_event_drop_delta < 0) {
        stack_event_drop_delta = 0
    }

    printf "Diagnostics soak summary\n"
    printf "samples,%.0f\n", sample_count
    printf "duration_ms,%.0f\n", duration_ms
    printf "sequence_first,%.0f\n", first_sequence
    printf "sequence_last,%.0f\n", last_sequence
    printf "max_usb_tx_depth,%.0f\n", max_usb_tx_depth
    printf "max_bt_tx_depth,%.0f\n", max_bt_tx_depth
    printf "max_usb_tx_high_watermark,%.0f\n", max_usb_tx_high_watermark
    printf "max_bt_tx_high_watermark,%.0f\n", max_bt_tx_high_watermark
    printf "max_stack_event_depth,%.0f\n", max_stack_event_depth
    printf "max_stack_event_high_watermark,%.0f\n", max_stack_event_high_watermark
    printf "usb_tx_dropped_delta,%.0f\n", usb_drop_delta
    printf "bt_tx_dropped_delta,%.0f\n", bt_drop_delta
    printf "stack_event_dropped_delta,%.0f\n", stack_event_drop_delta
    printf "reconnect_attempt_delta,%.0f\n", reconnect_attempt_delta
    printf "reconnect_success_delta,%.0f\n", reconnect_success_delta
    printf "reconnect_failure_delta,%.0f\n", reconnect_failure_delta

    if ((usb_drop_delta > 0) || (bt_drop_delta > 0) || (stack_event_drop_delta > 0)) {
        printf "result,warning_drops_detected\n"
    } else {
        printf "result,ok_no_drops\n"
    }

    gate_check("usb_tx_dropped_delta", usb_drop_delta, max_usb_drop_delta)
    gate_check("bt_tx_dropped_delta", bt_drop_delta, max_bt_drop_delta)
    gate_check("stack_event_dropped_delta", stack_event_drop_delta, max_stack_event_drop_delta)
    gate_check("reconnect_failure_delta", reconnect_failure_delta, max_reconnect_failure_delta)

    if (gate_enabled == 1) {
        if (gate_failed == 1) {
            printf "gate_result,fail\n"
            exit 3
        }

        printf "gate_result,pass\n"
    }
}
' "$input_path"
