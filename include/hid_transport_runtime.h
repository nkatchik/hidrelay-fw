#ifndef HIDRELAY_HID_TRANSPORT_RUNTIME_H
#define HIDRELAY_HID_TRANSPORT_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "hid_transport.h"

#define HID_TRANSPORT_RUNTIME_EVENT_QUEUE_SIZE 64U

typedef const uint8_t * (*hid_transport_runtime_descriptor_fn_t)(
    uint8_t interface_number,
    uint16_t * out_len,
    void * context
);

typedef struct {
    uint8_t event_queue_depth;
    uint8_t event_queue_high_watermark;
    uint32_t event_queue_dropped;
} hid_transport_runtime_queue_state_t;

typedef struct {
    uint8_t usb_interface_count;
    uint32_t usb_descriptor_generation;
    hid_transport_usb_interface_plan_t usb_interface_plan[HID_TRANSPORT_MAX_INTERFACE];
    /*
     * Cached remap profile per interface. The policy decision behind it is a
     * full report-descriptor parse; its inputs (descriptor bytes, protocol
     * mode) only change through set_usb_plan, which invalidates this cache.
     */
    uint8_t remap_profile[HID_TRANSPORT_MAX_INTERFACE];
    bool remap_profile_valid[HID_TRANSPORT_MAX_INTERFACE];
    hid_transport_event_t event_queue[HID_TRANSPORT_RUNTIME_EVENT_QUEUE_SIZE];
    uint8_t event_queue_head;
    uint8_t event_queue_tail;
    uint8_t event_queue_count;
    uint8_t event_queue_high_watermark;
    uint32_t event_queue_dropped;
} hid_transport_runtime_t;

void hid_transport_runtime_init(hid_transport_runtime_t * runtime);
bool hid_transport_runtime_set_usb_plan(
    hid_transport_runtime_t * runtime,
    uint8_t interface_count,
    uint32_t descriptor_generation,
    const hid_transport_usb_interface_plan_t * interface_plan,
    bool * out_descriptor_changed
);
uint8_t hid_transport_runtime_usb_interface_count(const hid_transport_runtime_t * runtime);
bool hid_transport_runtime_usb_interface_plan_get(
    const hid_transport_runtime_t * runtime,
    uint8_t interface_number,
    hid_transport_usb_interface_plan_t * out_plan
);
uint8_t hid_transport_runtime_usb_protocol_mode(
    const hid_transport_runtime_t * runtime,
    uint8_t interface_number
);
bool hid_transport_runtime_find_hid_cid_for_device(
    const hid_transport_runtime_t * runtime,
    const pair_device_id_t * device_id,
    uint16_t * out_hid_cid,
    uint8_t * out_bt_link_type
);
bool hid_transport_runtime_push_event(
    hid_transport_runtime_t * runtime,
    const hid_transport_event_t * event
);
bool hid_transport_runtime_take_event(
    hid_transport_runtime_t * runtime,
    hid_transport_event_t * out_event
);
bool hid_transport_runtime_ingest_usb_report(
    hid_transport_runtime_t * runtime,
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len,
    hid_transport_runtime_descriptor_fn_t descriptor_fn,
    void * descriptor_context
);
bool hid_transport_runtime_remap_bt_to_usb(
    hid_transport_runtime_t * runtime,
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len,
    hid_transport_runtime_descriptor_fn_t descriptor_fn,
    void * descriptor_context,
    uint8_t * out_report,
    uint16_t * out_report_len
);
bool hid_transport_runtime_queue_state_get(
    const hid_transport_runtime_t * runtime,
    hid_transport_runtime_queue_state_t * out_state
);

#endif
