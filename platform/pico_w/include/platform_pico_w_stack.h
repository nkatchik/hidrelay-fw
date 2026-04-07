#ifndef HIDRELAY_PLATFORM_PICO_W_STACK_H
#define HIDRELAY_PLATFORM_PICO_W_STACK_H

#include <stdbool.h>
#include <stdint.h>

#include "hid_transport.h"

typedef struct {
    uint8_t event_queue_depth;
    uint8_t event_queue_high_watermark;
    uint32_t event_queue_dropped;
    uint8_t connect_pending;
    uint8_t reconnect_pending;
    uint8_t connect_mode;
    uint8_t reconnect_attempt_index;
    uint8_t reconnect_attempt_count;
    uint8_t last_connect_status;
} pico_w_stack_event_telemetry_t;

bool pico_w_stack_init(bool radio_ready);
void pico_w_stack_poll(uint32_t now_ms);
void pico_w_stack_set_usb_plan(
    uint8_t interface_count,
    uint32_t descriptor_generation,
    const hid_transport_usb_interface_plan_t * interface_plan
);
void pico_w_stack_set_pairing_active(bool pairing_active);
bool pico_w_stack_request_reconnect(
    const pair_device_id_t * device_id,
    uint8_t bt_link_type_hint,
    uint8_t bt_addr_type_hint
);
bool pico_w_stack_forget_device(const pair_device_id_t * device_id);
uint8_t pico_w_stack_usb_interface_count(void);
const uint8_t * pico_w_stack_usb_report_descriptor(
    uint8_t interface_number,
    uint16_t * out_len
);
uint16_t pico_w_stack_usb_report_descriptor_len(uint8_t interface_number);
uint8_t pico_w_stack_usb_protocol_mode(uint8_t interface_number);
bool pico_w_stack_take_event(hid_transport_event_t * out_event);
void pico_w_stack_ingest_usb_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
);
bool pico_w_stack_send_usb_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
);
bool pico_w_stack_send_bt_report(
    uint16_t hid_cid,
    uint8_t bt_link_type,
    uint8_t protocol_mode,
    const uint8_t * report,
    uint16_t report_len
);
bool pico_w_stack_event_telemetry_get(pico_w_stack_event_telemetry_t * out_telemetry);

#endif
