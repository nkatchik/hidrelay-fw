#include "hid_report_remap.h"

#include <stddef.h>
#include <string.h>

#include "hid_transport.h"

enum {
    HID_REPORT_REMAP_BOOT_KEYBOARD_PAYLOAD_LEN = 8U,
    HID_REPORT_REMAP_BOOT_MOUSE_PAYLOAD_LEN = 3U,
    HID_REPORT_REMAP_BOOT_KEYBOARD_REPORT_ID = 1U,
    HID_REPORT_REMAP_BOOT_MOUSE_REPORT_ID = 1U,
};

static bool hid_report_remap_copy_report(
    const uint8_t * report,
    uint16_t report_len,
    uint8_t * out_report,
    uint16_t * out_report_len
) {
    if ((out_report == NULL) || (out_report_len == NULL)) {
        return false;
    }

    if ((report_len > HID_TRANSPORT_REPORT_MAX_LEN) || ((report_len > 0U) && (report == NULL))) {
        return false;
    }

    if (report_len > 0U) {
        (void)memcpy(out_report, report, report_len);
    }

    *out_report_len = report_len;
    return true;
}

static bool hid_report_remap_bt_to_usb_boot(
    uint16_t payload_len,
    const uint8_t * report,
    uint16_t report_len,
    uint8_t * out_report,
    uint16_t * out_report_len
) {
    if ((report == NULL) || (out_report == NULL) || (out_report_len == NULL)) {
        return false;
    }

    if (report_len == payload_len) {
        (void)memcpy(out_report, report, payload_len);
        *out_report_len = payload_len;
        return true;
    }

    if (report_len == (uint16_t)(payload_len + 1U)) {
        (void)memcpy(out_report, &report[1], payload_len);
        *out_report_len = payload_len;
        return true;
    }

    return false;
}

static bool hid_report_remap_usb_to_bt_boot(
    uint16_t payload_len,
    uint8_t report_id,
    uint8_t protocol_mode,
    const uint8_t * report,
    uint16_t report_len,
    uint8_t * out_report,
    uint16_t * out_report_len
) {
    if ((report == NULL) || (out_report == NULL) || (out_report_len == NULL)) {
        return false;
    }

    if (protocol_mode == HID_TRANSPORT_PROTOCOL_REPORT) {
        if (report_len == (uint16_t)(payload_len + 1U)) {
            out_report[0] = (report[0] == 0U) ? report_id : report[0];
            (void)memcpy(&out_report[1], &report[1], payload_len);
            *out_report_len = (uint16_t)(payload_len + 1U);
            return true;
        }

        if (report_len == payload_len) {
            out_report[0] = report_id;
            (void)memcpy(&out_report[1], report, payload_len);
            *out_report_len = (uint16_t)(payload_len + 1U);
            return true;
        }

        return false;
    }

    if (report_len == payload_len) {
        (void)memcpy(out_report, report, payload_len);
        *out_report_len = payload_len;
        return true;
    }

    if (report_len == (uint16_t)(payload_len + 1U)) {
        (void)memcpy(out_report, &report[1], payload_len);
        *out_report_len = payload_len;
        return true;
    }

    return false;
}

static bool hid_report_remap_usb_to_bt_boot_keyboard(
    uint8_t protocol_mode,
    const uint8_t * report,
    uint16_t report_len,
    uint8_t * out_report,
    uint16_t * out_report_len
) {
    if (hid_report_remap_usb_to_bt_boot(
            HID_REPORT_REMAP_BOOT_KEYBOARD_PAYLOAD_LEN,
            HID_REPORT_REMAP_BOOT_KEYBOARD_REPORT_ID,
            protocol_mode,
            report,
            report_len,
            out_report,
            out_report_len
        )) {
        return true;
    }

    return hid_report_remap_usb_to_bt_boot(
        1U,
        HID_REPORT_REMAP_BOOT_KEYBOARD_REPORT_ID,
        protocol_mode,
        report,
        report_len,
        out_report,
        out_report_len
    );
}

uint8_t hid_report_remap_profile_from_policy(const hid_report_policy_decision_t * decision) {
    if (decision == NULL) {
        return HID_REPORT_REMAP_PROFILE_NONE;
    }

    if (decision->source == HID_REPORT_DESCRIPTOR_SOURCE_FALLBACK_BOOT_KEYBOARD) {
        return HID_REPORT_REMAP_PROFILE_BOOT_KEYBOARD;
    }

    if (decision->source == HID_REPORT_DESCRIPTOR_SOURCE_FALLBACK_BOOT_MOUSE) {
        return HID_REPORT_REMAP_PROFILE_BOOT_MOUSE;
    }

    return HID_REPORT_REMAP_PROFILE_NONE;
}

bool hid_report_remap_bt_to_usb(
    uint8_t profile,
    const uint8_t * report,
    uint16_t report_len,
    uint8_t * out_report,
    uint16_t * out_report_len
) {
    if (profile == HID_REPORT_REMAP_PROFILE_BOOT_KEYBOARD) {
        return hid_report_remap_bt_to_usb_boot(
            HID_REPORT_REMAP_BOOT_KEYBOARD_PAYLOAD_LEN,
            report,
            report_len,
            out_report,
            out_report_len
        );
    }

    if (profile == HID_REPORT_REMAP_PROFILE_BOOT_MOUSE) {
        return hid_report_remap_bt_to_usb_boot(
            HID_REPORT_REMAP_BOOT_MOUSE_PAYLOAD_LEN,
            report,
            report_len,
            out_report,
            out_report_len
        );
    }

    return hid_report_remap_copy_report(report, report_len, out_report, out_report_len);
}

bool hid_report_remap_usb_to_bt(
    uint8_t profile,
    uint8_t protocol_mode,
    const uint8_t * report,
    uint16_t report_len,
    uint8_t * out_report,
    uint16_t * out_report_len
) {
    if (profile == HID_REPORT_REMAP_PROFILE_BOOT_KEYBOARD) {
        return hid_report_remap_usb_to_bt_boot_keyboard(
            protocol_mode,
            report,
            report_len,
            out_report,
            out_report_len
        );
    }

    if (profile == HID_REPORT_REMAP_PROFILE_BOOT_MOUSE) {
        return hid_report_remap_usb_to_bt_boot(
            HID_REPORT_REMAP_BOOT_MOUSE_PAYLOAD_LEN,
            HID_REPORT_REMAP_BOOT_MOUSE_REPORT_ID,
            protocol_mode,
            report,
            report_len,
            out_report,
            out_report_len
        );
    }

    return hid_report_remap_copy_report(report, report_len, out_report, out_report_len);
}
