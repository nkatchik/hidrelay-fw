#ifndef HIDRELAY_HID_REPORT_REMAP_H
#define HIDRELAY_HID_REPORT_REMAP_H

#include <stdbool.h>
#include <stdint.h>

#include "hid_report_policy.h"

#define HID_REPORT_REMAP_PROFILE_NONE 0U
#define HID_REPORT_REMAP_PROFILE_BOOT_KEYBOARD 1U
#define HID_REPORT_REMAP_PROFILE_BOOT_MOUSE 2U

uint8_t hid_report_remap_profile_from_policy(const hid_report_policy_decision_t * decision);

bool hid_report_remap_bt_to_usb(
    uint8_t profile,
    const uint8_t * report,
    uint16_t report_len,
    uint8_t * out_report,
    uint16_t * out_report_len
);

bool hid_report_remap_usb_to_bt(
    uint8_t profile,
    uint8_t protocol_mode,
    const uint8_t * report,
    uint16_t report_len,
    uint8_t * out_report,
    uint16_t * out_report_len
);

#endif
