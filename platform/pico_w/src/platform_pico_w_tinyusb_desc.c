#ifdef APP_PICO_HAS_TINYUSB

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tusb.h"

enum {
    HIDRELAY_USB_VID = 0x2E8AU,
    HIDRELAY_USB_PID = 0x4001U,
    HIDRELAY_USB_BCD = 0x0100U,
    HIDRELAY_HID_EP_IN = 0x81U,
    HIDRELAY_HID_EP_OUT = 0x01U,
    HIDRELAY_HID_EP_SIZE = 16U,
    HIDRELAY_HID_EP_INTERVAL_MS = 4U,
    HIDRELAY_STRING_LIMIT = 31U
};

enum {
    HIDRELAY_ITF_HID = 0U,
    HIDRELAY_ITF_TOTAL = 1U
};

#define HIDRELAY_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static const tusb_desc_device_t g_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200U,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = HIDRELAY_USB_VID,
    .idProduct = HIDRELAY_USB_PID,
    .bcdDevice = HIDRELAY_USB_BCD,
    .iManufacturer = 1U,
    .iProduct = 2U,
    .iSerialNumber = 3U,
    .bNumConfigurations = 1U,
};

static const uint8_t g_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(HIDRELAY_HID_EP_SIZE)
};

static const uint8_t g_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1U, HIDRELAY_ITF_TOTAL, 0U, HIDRELAY_CONFIG_TOTAL_LEN, 0U, 100U),
    TUD_HID_INOUT_DESCRIPTOR(HIDRELAY_ITF_HID, 0U, HID_ITF_PROTOCOL_NONE, sizeof(g_hid_report_desc),
                             HIDRELAY_HID_EP_OUT, HIDRELAY_HID_EP_IN, HIDRELAY_HID_EP_SIZE,
                             HIDRELAY_HID_EP_INTERVAL_MS)
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&g_device_desc;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return g_hid_report_desc;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return g_config_desc;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    static uint16_t descriptor[HIDRELAY_STRING_LIMIT + 1U];
    static const char *const strings[] = {
        "hidrelay-fw",
        "HID Relay Hub",
        "00000001",
    };
    uint8_t char_count = 0U;
    const char *text = NULL;

    (void)langid;

    if (index == 0U) {
        descriptor[1] = 0x0409U;
        descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8U) | (2U + 2U));
        return descriptor;
    }

    if ((index - 1U) >= (sizeof(strings) / sizeof(strings[0]))) {
        return NULL;
    }

    text = strings[index - 1U];
    char_count = (uint8_t)strlen(text);

    if (char_count > HIDRELAY_STRING_LIMIT) {
        char_count = HIDRELAY_STRING_LIMIT;
    }

    for (uint8_t i = 0U; i < char_count; i++) {
        descriptor[i + 1U] = (uint8_t)text[i];
    }

    descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8U) | (2U + (char_count * 2U)));
    return descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0U;
}

void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

#endif
