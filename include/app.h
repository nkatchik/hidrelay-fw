#ifndef HIDRELAY_APP_H
#define HIDRELAY_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "bt_manager.h"
#include "button_fsm.h"
#include "hid_transport.h"
#include "led_ui.h"
#include "pair_db.h"
#include "usb_bridge.h"

typedef struct {
    bool button_pressed;
    uint32_t now_ms;
    hid_transport_event_t transport_event;
} app_input_t;

typedef struct {
    bool led_on;
    bool pairing_active;
    uint32_t sleep_ms;
    uint8_t usb_interface_count;
    uint32_t usb_descriptor_generation;
    uint8_t usb_tx_queue_depth;
    uint8_t bt_tx_queue_depth;
    uint8_t usb_tx_queue_high_watermark;
    uint8_t bt_tx_queue_high_watermark;
    uint32_t usb_tx_dropped;
    uint32_t bt_tx_dropped;
    uint8_t active_device_count;
    hid_transport_usb_interface_plan_t usb_interface_plan[HID_TRANSPORT_MAX_INTERFACE];
    hid_transport_reconnect_request_t reconnect_request;
    hid_transport_diag_snapshot_t diag;
    hid_transport_usb_tx_t usb_tx;
    hid_transport_bt_tx_t bt_tx;
    bool pair_db_dirty;
} app_output_t;

typedef struct {
    button_fsm_t button_fsm;
    led_ui_t led_ui;
    pair_db_t pair_db;
    bt_manager_t bt_manager;
    usb_bridge_t usb_bridge;
    bool reconnect_inflight;
    pair_device_id_t reconnect_device_id;
    uint32_t reconnect_started_ms;
    uint32_t reconnect_attempt_count;
    uint32_t reconnect_success_count;
    uint32_t reconnect_failure_count;
    uint8_t reconnect_last_result;
    uint8_t reconnect_last_status_code;
} app_t;

void app_init(app_t *app, const pair_db_t *initial_pair_db);
void app_tick(app_t *app, const app_input_t *input, app_output_t *output);

#endif
