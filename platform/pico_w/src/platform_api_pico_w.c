#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "platform_api.h"
#include "platform_pico_w_hw.h"
#include "platform_pico_w_pair_store.h"
#include "platform_pico_w_stack.h"
#include "platform_pico_w_state.h"
#include "platform_pico_w_tinyusb_runtime.h"

static pico_w_state_t g_state = {
    .initialized = false,
};

#if defined(APP_PICO_HAS_TELEMETRY)
enum {
    PICO_W_DIAG_QUEUE_SIZE = 16U,
    PICO_W_DIAG_FRAME_VERSION = 1U,
    PICO_W_DIAG_FRAME_MAGIC_0 = 0x48U,
    PICO_W_DIAG_FRAME_MAGIC_1 = 0x52U,
    PICO_W_DIAG_FRAME_PAYLOAD_LEN = 39U,
    PICO_W_DIAG_FRAME_LEN = 4U + PICO_W_DIAG_FRAME_PAYLOAD_LEN,
};
static hid_transport_diag_snapshot_t g_last_diag = {0};
static bool g_last_diag_valid = false;
static hid_transport_diag_snapshot_t g_diag_queue[PICO_W_DIAG_QUEUE_SIZE] = {0};
static uint8_t g_diag_queue_head = 0U;
static uint8_t g_diag_queue_tail = 0U;
static uint8_t g_diag_queue_count = 0U;
static uint32_t g_diag_sequence = 0U;
#endif

#if defined(APP_PICO_HAS_TELEMETRY)
static void pico_w_diag_put_u32(
    uint8_t * frame,
    uint16_t * offset,
    uint32_t value
) {
    uint16_t write_offset = 0U;

    if ((frame == NULL) || (offset == NULL)) {
        return;
    }

    write_offset = *offset;
    frame[write_offset++] = (uint8_t)(value & 0xFFU);
    frame[write_offset++] = (uint8_t)((value >> 8U) & 0xFFU);
    frame[write_offset++] = (uint8_t)((value >> 16U) & 0xFFU);
    frame[write_offset++] = (uint8_t)((value >> 24U) & 0xFFU);
    *offset = write_offset;
}

static uint16_t pico_w_diag_encode_frame(
    const hid_transport_diag_snapshot_t * diag,
    uint32_t sequence,
    uint8_t * frame,
    uint16_t frame_capacity
) {
    uint16_t offset = 0U;

    if ((diag == NULL) || (frame == NULL) || (frame_capacity < PICO_W_DIAG_FRAME_LEN)) {
        return 0U;
    }

    frame[offset++] = PICO_W_DIAG_FRAME_MAGIC_0;
    frame[offset++] = PICO_W_DIAG_FRAME_MAGIC_1;
    frame[offset++] = PICO_W_DIAG_FRAME_VERSION;
    frame[offset++] = PICO_W_DIAG_FRAME_PAYLOAD_LEN;

    pico_w_diag_put_u32(frame, &offset, sequence);
    frame[offset++] = diag->bt_state;
    frame[offset++] = diag->active_device_count;
    frame[offset++] = diag->usb_interface_count;
    frame[offset++] = diag->usb_tx_depth;
    frame[offset++] = diag->bt_tx_depth;
    frame[offset++] = diag->usb_tx_high_watermark;
    frame[offset++] = diag->bt_tx_high_watermark;
    frame[offset++] = diag->reconnect_last_result;
    frame[offset++] = diag->reconnect_last_status_code;

    pico_w_diag_put_u32(frame, &offset, diag->usb_tx_dropped);
    pico_w_diag_put_u32(frame, &offset, diag->bt_tx_dropped);
    pico_w_diag_put_u32(frame, &offset, diag->reconnect_attempt_count);
    pico_w_diag_put_u32(frame, &offset, diag->reconnect_success_count);
    pico_w_diag_put_u32(frame, &offset, diag->reconnect_failure_count);
    frame[offset++] = diag->stack_event_depth;
    frame[offset++] = diag->stack_event_high_watermark;
    pico_w_diag_put_u32(frame, &offset, diag->stack_event_dropped);

    return offset;
}

static void pico_w_diag_send_usb(const hid_transport_diag_snapshot_t * diag) {
#if defined(APP_PICO_HAS_DIAG_CDC)
    uint8_t frame[PICO_W_DIAG_FRAME_LEN] = {0};
    uint16_t frame_len = 0U;

    if (diag == NULL) {
        return;
    }

    g_diag_sequence = g_diag_sequence + 1U;
    frame_len = pico_w_diag_encode_frame(diag, g_diag_sequence, frame, sizeof(frame));

    if (frame_len == 0U) {
        return;
    }

    (void)pico_w_tinyusb_runtime_diag_write(frame, frame_len);
#else
    (void)diag;
#endif
}

static void pico_w_diag_queue_reset(void) {
    (void)memset(g_diag_queue, 0, sizeof(g_diag_queue));
    g_diag_queue_head = 0U;
    g_diag_queue_tail = 0U;
    g_diag_queue_count = 0U;
    g_diag_sequence = 0U;
}

static void pico_w_diag_queue_push(const hid_transport_diag_snapshot_t * diag) {
    if (diag == NULL) {
        return;
    }

    if (g_diag_queue_count >= PICO_W_DIAG_QUEUE_SIZE) {
        g_diag_queue_head = (uint8_t)((g_diag_queue_head + 1U) % PICO_W_DIAG_QUEUE_SIZE);
        g_diag_queue_count = (uint8_t)(g_diag_queue_count - 1U);
    }

    g_diag_queue[g_diag_queue_tail] = *diag;
    g_diag_queue_tail = (uint8_t)((g_diag_queue_tail + 1U) % PICO_W_DIAG_QUEUE_SIZE);
    g_diag_queue_count = (uint8_t)(g_diag_queue_count + 1U);
}

static void pico_w_diag_publish(const hid_transport_diag_snapshot_t * diag) {
    if (diag == NULL) {
        return;
    }

    if (g_last_diag_valid && (memcmp(&g_last_diag, diag, sizeof(*diag)) == 0)) {
        return;
    }

    pico_w_diag_queue_push(diag);
    pico_w_diag_send_usb(diag);

    printf(
        "[diag] bt_state=%u active=%u usb_itf=%u usb_q=%u bt_q=%u ev_q=%u ev_q_hw=%u "
        "usb_drop=%lu bt_drop=%lu ev_drop=%lu r_attempt=%lu "
        "r_success=%lu r_fail=%lu r_last=%u r_status=%u\n",
        diag->bt_state,
        diag->active_device_count,
        diag->usb_interface_count,
        diag->usb_tx_depth,
        diag->bt_tx_depth,
        diag->stack_event_depth,
        diag->stack_event_high_watermark,
        (unsigned long)diag->usb_tx_dropped,
        (unsigned long)diag->bt_tx_dropped,
        (unsigned long)diag->stack_event_dropped,
        (unsigned long)diag->reconnect_attempt_count,
        (unsigned long)diag->reconnect_success_count,
        (unsigned long)diag->reconnect_failure_count,
        diag->reconnect_last_result,
        diag->reconnect_last_status_code
    );
    g_last_diag = *diag;
    g_last_diag_valid = true;
}
#else
static void pico_w_diag_publish(const hid_transport_diag_snapshot_t * diag) {
    (void)diag;
}
#endif

static void pico_w_factory_reset(void) {
    (void)pico_w_pair_store_factory_reset_all();
    watchdog_reboot(0U, 0U, 0U);

    for (;;) {
        tight_loop_contents();
    }
}

bool platform_init(void) {
#if defined(APP_PICO_HAS_TELEMETRY)
    stdio_init_all();
    pico_w_diag_queue_reset();
    g_last_diag_valid = false;
    (void)memset(&g_last_diag, 0, sizeof(g_last_diag));
#endif
    pico_w_state_reset(&g_state);

    if (!pico_w_hw_init_radio()) {
        return false;
    }

    if (!pico_w_stack_init()) {
        return false;
    }

    pico_w_state_mark_initialized(&g_state);
    return true;
}

void platform_poll(platform_input_t * input) {
    if ((input == NULL) || !pico_w_state_is_initialized(&g_state)) {
        return;
    }

    input->button_pressed = pico_w_hw_bootsel_pressed();
    input->uptime_ms = pico_w_hw_uptime_ms();
    input->transport_event.type = HID_TRANSPORT_EVENT_NONE;

    if (!pico_w_stack_take_event(&input->transport_event)) {
        input->transport_event.type = HID_TRANSPORT_EVENT_NONE;
    }

    pico_w_stack_poll(input->uptime_ms);
}

void platform_apply(const platform_output_t * output) {
#if defined(APP_PICO_HAS_TELEMETRY)
    pico_w_stack_event_telemetry_t stack_telemetry = {0};
    hid_transport_diag_snapshot_t diag = {0};
#endif
    if ((output == NULL) || !pico_w_state_is_initialized(&g_state)) {
        return;
    }

    if (output->forget_request.valid) {
        (void)pico_w_stack_forget_device(&output->forget_request.device_id);
    }

    pico_w_stack_set_usb_plan(
        output->usb_interface_count,
        output->usb_descriptor_generation,
        output->usb_interface_plan
    );
    pico_w_stack_set_pairing_active(output->pairing_active);

    if (output->reconnect_request.valid) {
        (void)pico_w_stack_request_reconnect(&output->reconnect_request.device_id);
    }

    if (output->usb_tx.valid) {
        (void)pico_w_stack_send_usb_report(
            output->usb_tx.interface_number,
            output->usb_tx.report,
            output->usb_tx.report_len
        );
    }

    if (output->bt_tx.valid) {
        (void)pico_w_stack_send_bt_report(
            output->bt_tx.hid_cid,
            output->bt_tx.bt_link_type,
            output->bt_tx.protocol_mode,
            output->bt_tx.report,
            output->bt_tx.report_len
        );
    }

    if (output->factory_reset_requested) {
        pico_w_factory_reset();
        return;
    }

#if defined(APP_PICO_HAS_TELEMETRY)
    diag = output->diag;

    if (pico_w_stack_event_telemetry_get(&stack_telemetry)) {
        diag.stack_event_depth = stack_telemetry.event_queue_depth;
        diag.stack_event_high_watermark = stack_telemetry.event_queue_high_watermark;
        diag.stack_event_dropped = stack_telemetry.event_queue_dropped;
    }

    pico_w_diag_publish(&diag);
#else
    pico_w_diag_publish(&output->diag);
#endif

    pico_w_hw_set_led(output->led_on);
    pico_w_hw_sleep_ms(output->sleep_ms);
}

bool platform_pair_db_load(pair_db_t * db) {
    if (db == NULL) {
        return false;
    }

    return pico_w_pair_store_load(db);
}

bool platform_pair_db_save(const pair_db_t * db) {
    if (db == NULL) {
        return false;
    }

    return pico_w_pair_store_save(db);
}

bool platform_diag_take(hid_transport_diag_snapshot_t * out_diag) {
#if defined(APP_PICO_HAS_TELEMETRY)
    if ((out_diag == NULL)
        || !pico_w_state_is_initialized(&g_state)
        || (g_diag_queue_count == 0U)) {
        return false;
    }

    *out_diag = g_diag_queue[g_diag_queue_head];
    (void)memset(&g_diag_queue[g_diag_queue_head], 0, sizeof(g_diag_queue[g_diag_queue_head]));
    g_diag_queue_head = (uint8_t)((g_diag_queue_head + 1U) % PICO_W_DIAG_QUEUE_SIZE);
    g_diag_queue_count = (uint8_t)(g_diag_queue_count - 1U);
    return true;
#else
    (void)out_diag;
    return false;
#endif
}
