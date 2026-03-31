#ifndef HIDRELAY_HID_DEVICE_MAP_H
#define HIDRELAY_HID_DEVICE_MAP_H

#include <stdbool.h>
#include <stdint.h>

#define HID_DEVICE_MAP_PROFILE_NONE 0U
#define HID_DEVICE_MAP_PROFILE_APPLE_MAGIC_KEYBOARD 1U
#define HID_DEVICE_MAP_FN_MODE_MEDIA_DEFAULT 0U
#define HID_DEVICE_MAP_FN_MODE_FUNCTION_DEFAULT 1U

typedef struct {
    uint8_t profile;
    uint8_t fn_mode;
    bool fn_esc_latched;
} hid_device_map_state_t;

uint8_t hid_device_map_profile_detect(
    uint16_t vendor_id,
    uint16_t product_id
);

void hid_device_map_state_reset(
    hid_device_map_state_t * state,
    uint16_t vendor_id,
    uint16_t product_id
);

bool hid_device_map_track_fn_esc_toggle(
    hid_device_map_state_t * state,
    const uint8_t * report,
    uint16_t report_len
);

#endif
