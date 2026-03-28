#ifndef HIDRELAY_HID_TRANSPORT_H
#define HIDRELAY_HID_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "pair_db.h"

#define HID_TRANSPORT_REPORT_MAX_LEN 64U

typedef enum {
    HID_TRANSPORT_EVENT_NONE = 0,
    HID_TRANSPORT_EVENT_BT_HID_OPEN,
    HID_TRANSPORT_EVENT_BT_HID_CLOSE,
    HID_TRANSPORT_EVENT_BT_HID_REPORT,
    HID_TRANSPORT_EVENT_USB_HID_REPORT
} hid_transport_event_type_t;

typedef struct {
    hid_transport_event_type_t type;
    pair_device_id_t device_id;
    uint16_t hid_cid;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t report_descriptor_len;
    uint16_t report_len;
    uint8_t report[HID_TRANSPORT_REPORT_MAX_LEN];
    uint8_t interface_number;
} hid_transport_event_t;

typedef struct {
    bool valid;
    uint16_t hid_cid;
    uint16_t report_len;
    uint8_t report[HID_TRANSPORT_REPORT_MAX_LEN];
} hid_transport_bt_tx_t;

typedef struct {
    bool valid;
    uint8_t interface_number;
    uint16_t report_len;
    uint8_t report[HID_TRANSPORT_REPORT_MAX_LEN];
} hid_transport_usb_tx_t;

#endif
