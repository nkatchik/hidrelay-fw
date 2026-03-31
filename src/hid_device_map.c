#include "hid_device_map.h"

#include <stddef.h>

enum {
    HID_DEVICE_MAP_VENDOR_APPLE = 0x05ACU,
    HID_DEVICE_MAP_USAGE_ESCAPE = 0x29U,
    /*
     * Apple keyboards expose Fn differently across generations. We start with
     * a conservative surrogate (0x3F) and keep this isolated for iterative tuning.
     */
    HID_DEVICE_MAP_USAGE_FN_SURROGATE = 0x3FU,
};

static bool hid_device_map_apple_magic_keyboard_product(uint16_t product_id) {
    switch (product_id) {
        case 0x0267U:
        case 0x026CU:
        case 0x029CU:
            return true;
        default:
            return false;
    }
}

uint8_t hid_device_map_profile_detect(
    uint16_t vendor_id,
    uint16_t product_id
) {
    if ((vendor_id == HID_DEVICE_MAP_VENDOR_APPLE)
        && hid_device_map_apple_magic_keyboard_product(product_id)) {
        return HID_DEVICE_MAP_PROFILE_APPLE_MAGIC_KEYBOARD;
    }

    return HID_DEVICE_MAP_PROFILE_NONE;
}

void hid_device_map_state_reset(
    hid_device_map_state_t * state,
    uint16_t vendor_id,
    uint16_t product_id
) {
    if (state == NULL) {
        return;
    }

    state->profile = hid_device_map_profile_detect(vendor_id, product_id);
    state->fn_mode = (state->profile == HID_DEVICE_MAP_PROFILE_APPLE_MAGIC_KEYBOARD)
        ? HID_DEVICE_MAP_FN_MODE_MEDIA_DEFAULT
        : HID_DEVICE_MAP_FN_MODE_FUNCTION_DEFAULT;
    state->fn_esc_latched = false;
}

static bool hid_device_map_report_has_usage(
    const uint8_t * report,
    uint16_t report_len,
    uint8_t usage
) {
    uint16_t index = 0U;

    if ((report == NULL) || (report_len == 0U)) {
        return false;
    }

    for (index = 0U; index < report_len; index++) {
        if (report[index] == usage) {
            return true;
        }
    }

    return false;
}

bool hid_device_map_track_fn_esc_toggle(
    hid_device_map_state_t * state,
    const uint8_t * report,
    uint16_t report_len
) {
    bool combo_active = false;

    if ((state == NULL)
        || (state->profile != HID_DEVICE_MAP_PROFILE_APPLE_MAGIC_KEYBOARD)
        || (report == NULL)
        || (report_len == 0U)) {
        return false;
    }

    combo_active = hid_device_map_report_has_usage(report, report_len, HID_DEVICE_MAP_USAGE_ESCAPE)
        && hid_device_map_report_has_usage(report, report_len, HID_DEVICE_MAP_USAGE_FN_SURROGATE);

    if (combo_active && !state->fn_esc_latched) {
        state->fn_mode = (state->fn_mode == HID_DEVICE_MAP_FN_MODE_MEDIA_DEFAULT)
            ? HID_DEVICE_MAP_FN_MODE_FUNCTION_DEFAULT
            : HID_DEVICE_MAP_FN_MODE_MEDIA_DEFAULT;
        state->fn_esc_latched = true;
        return true;
    }

    if (!combo_active) {
        state->fn_esc_latched = false;
    }

    return false;
}
