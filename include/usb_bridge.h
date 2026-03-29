#ifndef HIDRELAY_USB_BRIDGE_H
#define HIDRELAY_USB_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "bt_manager.h"
#include "hid_transport.h"
#include "pair_db.h"

#define USB_BRIDGE_MAX_INTERFACE BT_MANAGER_MAX_ACTIVE_DEVICE

typedef struct {
    bool used;
    uint8_t interface_number;
    uint8_t endpoint_in;
    uint8_t endpoint_out;
    uint16_t hid_cid;
    uint16_t report_descriptor_len;
    uint8_t protocol_mode;
    pair_device_id_t device_id;
} usb_bridge_interface_t;

typedef struct {
    uint8_t usb_tx_depth;
    uint8_t bt_tx_depth;
    uint8_t usb_tx_high_watermark;
    uint8_t bt_tx_high_watermark;
    uint32_t usb_tx_enqueued;
    uint32_t bt_tx_enqueued;
    uint32_t usb_tx_dequeued;
    uint32_t bt_tx_dequeued;
    uint32_t usb_tx_dropped;
    uint32_t bt_tx_dropped;
} usb_bridge_telemetry_t;

typedef struct {
    usb_bridge_interface_t interface_slot[USB_BRIDGE_MAX_INTERFACE];
    uint8_t exported_interface_count;
    uint32_t descriptor_generation;
    hid_transport_usb_tx_t usb_tx_queue[USB_BRIDGE_MAX_INTERFACE];
    hid_transport_bt_tx_t bt_tx_queue[USB_BRIDGE_MAX_INTERFACE];
    uint8_t usb_tx_queue_head;
    uint8_t usb_tx_queue_tail;
    uint8_t usb_tx_queue_count;
    uint8_t bt_tx_queue_head;
    uint8_t bt_tx_queue_tail;
    uint8_t bt_tx_queue_count;
    usb_bridge_telemetry_t telemetry;
} usb_bridge_t;

void usb_bridge_init(usb_bridge_t * bridge);
void usb_bridge_sync_from_pair_db(
    usb_bridge_t * bridge,
    const pair_db_t * pair_db
);
void usb_bridge_sync_from_bt_manager(
    usb_bridge_t * bridge,
    const bt_manager_t * manager
);
bool usb_bridge_ingest_bt_report(
    usb_bridge_t * bridge,
    uint16_t hid_cid,
    const uint8_t * report,
    uint16_t report_len
);
bool usb_bridge_ingest_usb_report(
    usb_bridge_t * bridge,
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
);
bool usb_bridge_take_usb_tx(
    usb_bridge_t * bridge,
    hid_transport_usb_tx_t * out_tx
);
bool usb_bridge_take_bt_tx(
    usb_bridge_t * bridge,
    hid_transport_bt_tx_t * out_tx
);
bool usb_bridge_telemetry_get(
    const usb_bridge_t * bridge,
    usb_bridge_telemetry_t * out_telemetry
);
void usb_bridge_tick(
    usb_bridge_t * bridge,
    uint32_t now_ms
);
uint8_t usb_bridge_interface_count(const usb_bridge_t * bridge);
uint32_t usb_bridge_descriptor_generation(const usb_bridge_t * bridge);
bool usb_bridge_interface_get(
    const usb_bridge_t * bridge,
    uint8_t index,
    usb_bridge_interface_t * out_interface
);

#endif
