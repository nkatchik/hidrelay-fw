#ifndef HIDRELAY_PLATFORM_API_H
#define HIDRELAY_PLATFORM_API_H

#include <stdbool.h>
#include <stdint.h>

#include "hid_transport.h"
#include "pair_db.h"

typedef struct {
    bool button_pressed;
    uint32_t uptime_ms;
    hid_transport_event_t transport_event;
} platform_input_t;

typedef struct {
    bool led_on;
    bool pairing_active;
    uint8_t pairing_link_type;
    uint32_t sleep_us;
    uint8_t usb_interface_count;
    uint32_t usb_descriptor_generation;
    hid_transport_usb_interface_plan_t usb_interface_plan[HID_TRANSPORT_MAX_INTERFACE];
    hid_transport_reconnect_request_t reconnect_request;
    hid_transport_forget_request_t forget_request;
    hid_transport_diag_snapshot_t diag;
    hid_transport_usb_tx_t usb_tx;
    hid_transport_bt_tx_t bt_tx;
    bool factory_reset_requested;
} platform_output_t;

bool platform_init(void);
void platform_poll(platform_input_t * input);
void platform_apply(const platform_output_t * output);
bool platform_pair_db_load(pair_db_t * db);
bool platform_pair_db_save(const pair_db_t * db);
bool platform_diag_take(hid_transport_diag_snapshot_t * out_diag);

#endif
