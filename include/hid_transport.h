#ifndef HIDRELAY_HID_TRANSPORT_H
#define HIDRELAY_HID_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "pair_db.h"

#define HID_TRANSPORT_REPORT_MAX_LEN 64U
#define HID_TRANSPORT_MAX_INTERFACE PAIR_DB_MAX_DEVICE
#define HID_TRANSPORT_PROTOCOL_UNKNOWN 0U
#define HID_TRANSPORT_PROTOCOL_BOOT 1U
#define HID_TRANSPORT_PROTOCOL_REPORT 2U
#define HID_TRANSPORT_BT_LINK_UNKNOWN 0U
#define HID_TRANSPORT_BT_LINK_CLASSIC 1U
#define HID_TRANSPORT_BT_LINK_LE 2U
#define HID_TRANSPORT_RECONNECT_RESULT_NONE 0U
#define HID_TRANSPORT_RECONNECT_RESULT_REQUESTED 1U
#define HID_TRANSPORT_RECONNECT_RESULT_SUCCESS 2U
#define HID_TRANSPORT_RECONNECT_RESULT_TIMEOUT 3U
#define HID_TRANSPORT_RECONNECT_RESULT_NO_CANDIDATE 4U
#define HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED 5U
#define HID_TRANSPORT_RECONNECT_RESULT_CONNECT_FAILED 6U
#define HID_TRANSPORT_RECONNECT_RESULT_AUTH_FAILED 7U

typedef enum {
    HID_TRANSPORT_EVENT_NONE = 0,
    HID_TRANSPORT_EVENT_BT_HID_OPEN,
    HID_TRANSPORT_EVENT_BT_HID_CLOSE,
    HID_TRANSPORT_EVENT_BT_HID_REPORT,
    HID_TRANSPORT_EVENT_USB_HID_REPORT,
    HID_TRANSPORT_EVENT_BT_HID_DESCRIPTOR,
    HID_TRANSPORT_EVENT_BT_HID_PROTOCOL,
    HID_TRANSPORT_EVENT_RECONNECT_RESULT
} hid_transport_event_type_t;

typedef enum {
    HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN = HID_TRANSPORT_BT_LINK_UNKNOWN,
    HID_TRANSPORT_BT_LINK_TYPE_CLASSIC = HID_TRANSPORT_BT_LINK_CLASSIC,
    HID_TRANSPORT_BT_LINK_TYPE_LE = HID_TRANSPORT_BT_LINK_LE
} hid_transport_bt_link_type_t;

typedef struct {
    hid_transport_event_type_t type;
    pair_device_id_t device_id;
    uint16_t hid_cid;
    uint8_t bt_link_type;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t report_descriptor_len;
    uint8_t protocol_mode;
    uint8_t reconnect_result;
    uint8_t status_code;
    uint16_t report_len;
    uint8_t report[HID_TRANSPORT_REPORT_MAX_LEN];
    uint8_t interface_number;
} hid_transport_event_t;

typedef struct {
    bool valid;
    uint16_t hid_cid;
    uint8_t bt_link_type;
    uint8_t protocol_mode;
    uint16_t report_len;
    uint8_t report[HID_TRANSPORT_REPORT_MAX_LEN];
} hid_transport_bt_tx_t;

typedef struct {
    bool valid;
    uint8_t interface_number;
    uint16_t report_len;
    uint8_t report[HID_TRANSPORT_REPORT_MAX_LEN];
} hid_transport_usb_tx_t;

typedef struct {
    uint16_t hid_cid;
    uint8_t bt_link_type;
    uint16_t report_descriptor_len;
    uint8_t protocol_mode;
    pair_device_id_t device_id;
} hid_transport_usb_interface_plan_t;

typedef struct {
    bool valid;
    pair_device_id_t device_id;
} hid_transport_reconnect_request_t;

typedef struct {
    bool valid;
    pair_device_id_t device_id;
} hid_transport_forget_request_t;

typedef struct {
    uint8_t bt_state;
    uint8_t active_device_count;
    uint8_t usb_interface_count;
    uint8_t usb_tx_depth;
    uint8_t bt_tx_depth;
    uint8_t usb_tx_high_watermark;
    uint8_t bt_tx_high_watermark;
    uint8_t stack_event_depth;
    uint8_t stack_event_high_watermark;
    uint32_t usb_tx_dropped;
    uint32_t bt_tx_dropped;
    uint32_t stack_event_dropped;
    uint32_t reconnect_attempt_count;
    uint32_t reconnect_success_count;
    uint32_t reconnect_failure_count;
    uint8_t reconnect_last_result;
    uint8_t reconnect_last_status_code;
} hid_transport_diag_snapshot_t;

#endif
