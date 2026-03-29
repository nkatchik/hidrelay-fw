#ifdef APP_PICO_HAS_TINYUSB

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform_pico_w_stack.h"
#include "tusb.h"

enum {
    HIDRELAY_USB_VID = 0x2E8AU,
    HIDRELAY_USB_PID = 0x4001U,
    HIDRELAY_USB_BCD = 0x0100U,
    HIDRELAY_HID_EP_IN = 0x81U,
    HIDRELAY_HID_EP_OUT = 0x01U,
    HIDRELAY_HID_EP_SIZE = 16U,
    HIDRELAY_HID_EP_INTERVAL_MS = 4U,
    HIDRELAY_STRING_LIMIT = 31U,
    HIDRELAY_MAX_INTERFACE = 8U,
    HIDRELAY_REPORT_DESC_MIN_LEN = 4U,
    HIDRELAY_REPORT_DESC_MAX_LEN = 1024U,
    HIDRELAY_REPORT_DESC_MAX_COLLECTION_DEPTH = 16U,
    HIDRELAY_REPORT_DESC_MAX_FIELD_BITS = 8192U,
    HIDRELAY_CONFIG_DESCRIPTOR_BASE_LEN = 9U,
    HIDRELAY_HID_INTERFACE_DESCRIPTOR_LEN = 9U + 9U + 7U + 7U
};

static uint8_t g_config_desc[HIDRELAY_CONFIG_DESCRIPTOR_BASE_LEN +
                             (HIDRELAY_MAX_INTERFACE * HIDRELAY_HID_INTERFACE_DESCRIPTOR_LEN)] = {0};
static uint16_t g_config_desc_len = HIDRELAY_CONFIG_DESCRIPTOR_BASE_LEN;

static const tusb_desc_device_t g_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200U,
    .bDeviceClass = 0U,
    .bDeviceSubClass = 0U,
    .bDeviceProtocol = 0U,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = HIDRELAY_USB_VID,
    .idProduct = HIDRELAY_USB_PID,
    .bcdDevice = HIDRELAY_USB_BCD,
    .iManufacturer = 1U,
    .iProduct = 2U,
    .iSerialNumber = 3U,
    .bNumConfigurations = 1U,
};

static const uint8_t g_hid_report_desc_generic[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(HIDRELAY_HID_EP_SIZE)
};

static uint32_t hidrelay_report_descriptor_read_u32(const uint8_t *data, uint8_t len) {
    uint32_t value = 0U;

    if ((data == NULL) || (len == 0U) || (len > 4U)) {
        return 0U;
    }

    for (uint8_t index = 0U; index < len; index++) {
        value |= ((uint32_t)data[index]) << (index * 8U);
    }

    return value;
}

static bool hidrelay_report_descriptor_supported(const uint8_t *descriptor, uint16_t descriptor_len) {
    uint16_t offset = 0U;
    uint8_t collection_depth = 0U;
    bool has_application_collection = false;
    uint32_t report_size = 0U;
    uint32_t report_count = 0U;

    if (descriptor == NULL) {
        return false;
    }

    if ((descriptor_len < HIDRELAY_REPORT_DESC_MIN_LEN) || (descriptor_len > HIDRELAY_REPORT_DESC_MAX_LEN)) {
        return false;
    }

    while (offset < descriptor_len) {
        const uint8_t prefix = descriptor[offset++];
        uint8_t data_len = 0U;
        uint8_t item_type = 0U;
        uint8_t item_tag = 0U;

        if (prefix == 0xFEU) {
            return false;
        }

        data_len = (uint8_t)(prefix & 0x03U);
        if (data_len == 3U) {
            data_len = 4U;
        }

        item_type = (uint8_t)((prefix >> 2U) & 0x03U);
        item_tag = (uint8_t)((prefix >> 4U) & 0x0FU);

        if ((uint16_t)(offset + data_len) > descriptor_len) {
            return false;
        }

        if (item_type == 0U) {
            if (item_tag == 0x0AU) {
                if (data_len != 1U) {
                    return false;
                }

                if (descriptor[offset] == 0x01U) {
                    has_application_collection = true;
                }

                if (collection_depth >= HIDRELAY_REPORT_DESC_MAX_COLLECTION_DEPTH) {
                    return false;
                }

                collection_depth = (uint8_t)(collection_depth + 1U);
            } else if (item_tag == 0x0CU) {
                if (data_len != 0U) {
                    return false;
                }

                if (collection_depth == 0U) {
                    return false;
                }

                collection_depth = (uint8_t)(collection_depth - 1U);
            }
        } else if (item_type == 1U) {
            if (item_tag == 0x07U) {
                report_size = hidrelay_report_descriptor_read_u32(&descriptor[offset], data_len);
            } else if (item_tag == 0x09U) {
                report_count = hidrelay_report_descriptor_read_u32(&descriptor[offset], data_len);
            }

            if ((report_size > 0U) && (report_count > 0U)) {
                if ((report_size > HIDRELAY_REPORT_DESC_MAX_FIELD_BITS) ||
                    (report_count > HIDRELAY_REPORT_DESC_MAX_FIELD_BITS) ||
                    (report_count > (HIDRELAY_REPORT_DESC_MAX_FIELD_BITS / report_size))) {
                    return false;
                }
            }
        }

        offset = (uint16_t)(offset + data_len);
    }

    return (collection_depth == 0U) && has_application_collection;
}

static uint8_t const *hidrelay_report_descriptor_for_interface(uint8_t instance, uint16_t *out_len) {
    uint16_t ignored_len = 0U;
    uint16_t *effective_len = out_len;
    const uint8_t *descriptor = NULL;

    if (effective_len == NULL) {
        effective_len = &ignored_len;
    }

    descriptor = pico_w_stack_usb_report_descriptor(instance, effective_len);

    if (hidrelay_report_descriptor_supported(descriptor, *effective_len)) {
        return descriptor;
    }

    *effective_len = (uint16_t)sizeof(g_hid_report_desc_generic);
    return g_hid_report_desc_generic;
}

static void hidrelay_descriptor_put_u16(uint8_t *buffer, uint16_t value) {
    if (buffer == NULL) {
        return;
    }

    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static uint16_t hidrelay_build_config_descriptor(uint8_t interface_count) {
    uint16_t offset = 0U;
    uint8_t index = 0U;

    if (interface_count > HIDRELAY_MAX_INTERFACE) {
        interface_count = HIDRELAY_MAX_INTERFACE;
    }

    g_config_desc[offset++] = 9U;
    g_config_desc[offset++] = TUSB_DESC_CONFIGURATION;
    hidrelay_descriptor_put_u16(&g_config_desc[offset],
                                (uint16_t)(HIDRELAY_CONFIG_DESCRIPTOR_BASE_LEN +
                                           (interface_count * HIDRELAY_HID_INTERFACE_DESCRIPTOR_LEN)));
    offset = (uint16_t)(offset + 2U);
    g_config_desc[offset++] = interface_count;
    g_config_desc[offset++] = 1U;
    g_config_desc[offset++] = 0U;
    g_config_desc[offset++] = 0x80U;
    g_config_desc[offset++] = 100U;

    for (index = 0U; index < interface_count; index++) {
        const uint8_t ep_out = (uint8_t)(HIDRELAY_HID_EP_OUT + index);
        const uint8_t ep_in = (uint8_t)(HIDRELAY_HID_EP_IN + index);
        uint16_t report_desc_len = 0U;

        (void)hidrelay_report_descriptor_for_interface(index, &report_desc_len);

        g_config_desc[offset++] = 9U;
        g_config_desc[offset++] = TUSB_DESC_INTERFACE;
        g_config_desc[offset++] = index;
        g_config_desc[offset++] = 0U;
        g_config_desc[offset++] = 2U;
        g_config_desc[offset++] = TUSB_CLASS_HID;
        g_config_desc[offset++] = 0U;
        g_config_desc[offset++] = HID_ITF_PROTOCOL_NONE;
        g_config_desc[offset++] = 0U;

        g_config_desc[offset++] = 9U;
        g_config_desc[offset++] = HID_DESC_TYPE_HID;
        hidrelay_descriptor_put_u16(&g_config_desc[offset], 0x0111U);
        offset = (uint16_t)(offset + 2U);
        g_config_desc[offset++] = 0U;
        g_config_desc[offset++] = 1U;
        g_config_desc[offset++] = HID_DESC_TYPE_REPORT;
        hidrelay_descriptor_put_u16(&g_config_desc[offset], report_desc_len);
        offset = (uint16_t)(offset + 2U);

        g_config_desc[offset++] = 7U;
        g_config_desc[offset++] = TUSB_DESC_ENDPOINT;
        g_config_desc[offset++] = ep_out;
        g_config_desc[offset++] = TUSB_XFER_INTERRUPT;
        hidrelay_descriptor_put_u16(&g_config_desc[offset], HIDRELAY_HID_EP_SIZE);
        offset = (uint16_t)(offset + 2U);
        g_config_desc[offset++] = HIDRELAY_HID_EP_INTERVAL_MS;

        g_config_desc[offset++] = 7U;
        g_config_desc[offset++] = TUSB_DESC_ENDPOINT;
        g_config_desc[offset++] = ep_in;
        g_config_desc[offset++] = TUSB_XFER_INTERRUPT;
        hidrelay_descriptor_put_u16(&g_config_desc[offset], HIDRELAY_HID_EP_SIZE);
        offset = (uint16_t)(offset + 2U);
        g_config_desc[offset++] = HIDRELAY_HID_EP_INTERVAL_MS;
    }

    return offset;
}

uint8_t const *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&g_device_desc;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    uint16_t report_len = 0U;
    return hidrelay_report_descriptor_for_interface(instance, &report_len);
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    uint8_t interface_count = 0U;

    (void)index;
    interface_count = pico_w_stack_usb_interface_count();
    g_config_desc_len = hidrelay_build_config_descriptor(interface_count);
    (void)g_config_desc_len;
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
    (void)report_id;
    (void)report_type;

    pico_w_stack_ingest_usb_report(instance, buffer, bufsize);
}

#endif
