#ifndef HIDRELAY_HID_REPORT_POLICY_H
#define HIDRELAY_HID_REPORT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define HID_REPORT_POLICY_ROLE_UNKNOWN 0U
#define HID_REPORT_POLICY_ROLE_KEYBOARD 1U
#define HID_REPORT_POLICY_ROLE_MOUSE 2U

#define HID_REPORT_POLICY_REJECT_NONE 0U
#define HID_REPORT_POLICY_REJECT_NULL_DESCRIPTOR 1U
#define HID_REPORT_POLICY_REJECT_LENGTH 2U
#define HID_REPORT_POLICY_REJECT_ITEM_FORMAT 3U
#define HID_REPORT_POLICY_REJECT_COLLECTION_DEPTH 4U
#define HID_REPORT_POLICY_REJECT_GLOBAL_STACK 5U
#define HID_REPORT_POLICY_REJECT_FIELD_BOUNDS 6U
#define HID_REPORT_POLICY_REJECT_REPORT_ID 7U
#define HID_REPORT_POLICY_REJECT_MISSING_INPUT 8U
#define HID_REPORT_POLICY_REJECT_MISSING_APPLICATION 9U
#define HID_REPORT_POLICY_REJECT_BOOT_WITH_REPORT_ID 10U

typedef enum {
    HID_REPORT_DESCRIPTOR_SOURCE_NATIVE = 0,
    HID_REPORT_DESCRIPTOR_SOURCE_FALLBACK_GENERIC,
    HID_REPORT_DESCRIPTOR_SOURCE_FALLBACK_BOOT_KEYBOARD,
    HID_REPORT_DESCRIPTOR_SOURCE_FALLBACK_BOOT_MOUSE
} hid_report_descriptor_source_t;

typedef struct {
    hid_report_descriptor_source_t source;
    bool native_supported;
    bool has_report_id;
    bool has_application_collection;
    bool has_input_item;
    uint8_t usage_role;
    uint8_t report_id_count;
    uint8_t reject_reason;
} hid_report_policy_decision_t;

void hid_report_policy_decide(
    const uint8_t * descriptor,
    uint16_t descriptor_len,
    uint8_t protocol_mode,
    hid_report_policy_decision_t * out_decision
);

#endif
