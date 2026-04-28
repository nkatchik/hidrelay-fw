#include "app_diag.h"

#include <stddef.h>
#include <string.h>

enum {
    APP_DIAG_QUEUE_SIZE = 16U,
    APP_DIAG_FRAME_VERSION = 1U,
    APP_DIAG_FRAME_MAGIC_0 = 0x48U,
    APP_DIAG_FRAME_MAGIC_1 = 0x52U,
    APP_DIAG_FRAME_PAYLOAD_LEN = 45U,
    APP_DIAG_FRAME_LEN = 4U + APP_DIAG_FRAME_PAYLOAD_LEN,
};

static hid_transport_diag_snapshot_t g_last_diag = {0};
static bool g_last_diag_valid = false;
static hid_transport_diag_snapshot_t g_diag_queue[APP_DIAG_QUEUE_SIZE] = {0};
static uint8_t g_diag_queue_head = 0U;
static uint8_t g_diag_queue_tail = 0U;
static uint8_t g_diag_queue_count = 0U;
static uint32_t g_diag_sequence = 0U;

static void app_diag_put_u32(
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

static void app_diag_queue_push(const hid_transport_diag_snapshot_t * diag) {
    if (diag == NULL) {
        return;
    }

    if (g_diag_queue_count >= APP_DIAG_QUEUE_SIZE) {
        g_diag_queue_head = (uint8_t)((g_diag_queue_head + 1U) % APP_DIAG_QUEUE_SIZE);
        g_diag_queue_count = (uint8_t)(g_diag_queue_count - 1U);
    }

    g_diag_queue[g_diag_queue_tail] = *diag;
    g_diag_queue_tail = (uint8_t)((g_diag_queue_tail + 1U) % APP_DIAG_QUEUE_SIZE);
    g_diag_queue_count = (uint8_t)(g_diag_queue_count + 1U);
}

void app_diag_init(void) {
    (void)memset(&g_last_diag, 0, sizeof(g_last_diag));
    g_last_diag_valid = false;
    (void)memset(g_diag_queue, 0, sizeof(g_diag_queue));
    g_diag_queue_head = 0U;
    g_diag_queue_tail = 0U;
    g_diag_queue_count = 0U;
    g_diag_sequence = 0U;
}

void app_diag_publish(const hid_transport_diag_snapshot_t * diag) {
    const bool changed =
        (diag != NULL) && (!g_last_diag_valid || (memcmp(&g_last_diag, diag, sizeof(*diag)) != 0));

    if (!changed) {
        return;
    }

    app_diag_queue_push(diag);
    g_last_diag = *diag;
    g_last_diag_valid = true;
}

bool app_diag_take(hid_transport_diag_snapshot_t * out_diag) {
    if ((out_diag == NULL) || (g_diag_queue_count == 0U)) {
        return false;
    }

    *out_diag = g_diag_queue[g_diag_queue_head];
    (void)memset(&g_diag_queue[g_diag_queue_head], 0, sizeof(g_diag_queue[g_diag_queue_head]));
    g_diag_queue_head = (uint8_t)((g_diag_queue_head + 1U) % APP_DIAG_QUEUE_SIZE);
    g_diag_queue_count = (uint8_t)(g_diag_queue_count - 1U);
    return true;
}

uint16_t app_diag_encode_frame(
    const hid_transport_diag_snapshot_t * diag,
    uint8_t * frame,
    uint16_t frame_capacity
) {
    uint16_t offset = 0U;

    if ((diag == NULL) || (frame == NULL) || (frame_capacity < APP_DIAG_FRAME_LEN)) {
        return 0U;
    }

    g_diag_sequence = g_diag_sequence + 1U;

    frame[offset++] = APP_DIAG_FRAME_MAGIC_0;
    frame[offset++] = APP_DIAG_FRAME_MAGIC_1;
    frame[offset++] = APP_DIAG_FRAME_VERSION;
    frame[offset++] = APP_DIAG_FRAME_PAYLOAD_LEN;

    app_diag_put_u32(frame, &offset, g_diag_sequence);
    frame[offset++] = diag->bt_state;
    frame[offset++] = diag->active_device_count;
    frame[offset++] = diag->usb_interface_count;
    frame[offset++] = diag->usb_tx_depth;
    frame[offset++] = diag->bt_tx_depth;
    frame[offset++] = diag->usb_tx_high_watermark;
    frame[offset++] = diag->bt_tx_high_watermark;
    frame[offset++] = diag->reconnect_last_result;
    frame[offset++] = diag->reconnect_last_status_code;

    app_diag_put_u32(frame, &offset, diag->usb_tx_dropped);
    app_diag_put_u32(frame, &offset, diag->bt_tx_dropped);
    app_diag_put_u32(frame, &offset, diag->reconnect_attempt_count);
    app_diag_put_u32(frame, &offset, diag->reconnect_success_count);
    app_diag_put_u32(frame, &offset, diag->reconnect_failure_count);
    frame[offset++] = diag->stack_event_depth;
    frame[offset++] = diag->stack_event_high_watermark;
    app_diag_put_u32(frame, &offset, diag->stack_event_dropped);
    frame[offset++] = diag->stack_connect_pending;
    frame[offset++] = diag->stack_reconnect_pending;
    frame[offset++] = diag->stack_connect_mode;
    frame[offset++] = diag->stack_reconnect_attempt_index;
    frame[offset++] = diag->stack_reconnect_attempt_count;
    frame[offset++] = diag->stack_last_connect_status;

    return offset;
}
