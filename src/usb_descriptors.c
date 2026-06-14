#ifdef APP_HAS_TINYUSB

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hid_report_policy.h"
#include "platform_usb_port.h"
#include "transport_stack.h"
#include "tusb.h"

#if defined(APP_HAS_DIAG_CDC)
#define HIDRELAY_CDC_DESCRIPTOR_LEN (8U + 9U + 5U + 5U + 4U + 5U + 7U + 9U + 7U + 7U)
#define HIDRELAY_CDC_INTERFACE_COUNT 2U
#else
#define HIDRELAY_CDC_DESCRIPTOR_LEN 0U
#define HIDRELAY_CDC_INTERFACE_COUNT 0U
#endif

enum {
    HIDRELAY_USB_VID = 0x2E8AU,
    HIDRELAY_USB_PID = 0x000AU,
    HIDRELAY_USB_BCD = 0x0100U,
    HIDRELAY_HID_EP_IN = 0x81U,
    HIDRELAY_HID_EP_OUT = 0x01U,
    HIDRELAY_HID_EP_SIZE = 64U,
    HIDRELAY_HID_EP_INTERVAL_MS = 1U,
#if defined(APP_HAS_DIAG_CDC)
    HIDRELAY_CDC_EP_NOTIF = 0x89U,
    HIDRELAY_CDC_EP_OUT = 0x09U,
    HIDRELAY_CDC_EP_IN = 0x8AU,
    HIDRELAY_CDC_EP_NOTIF_SIZE = 8U,
    HIDRELAY_CDC_EP_DATA_SIZE = 64U,
    HIDRELAY_CDC_EP_INTERVAL_MS = 16U,
    HIDRELAY_CDC_STRING_INDEX = 4U,
#endif
    HIDRELAY_STRING_LIMIT = 31U,
    HIDRELAY_MAX_INTERFACE = 8U,
    HIDRELAY_CONFIG_DESCRIPTOR_BASE_LEN = 9U,
    HIDRELAY_HID_INTERFACE_DESCRIPTOR_LEN = 9U + 9U + 7U + 7U
};

static uint8_t g_config_desc
    [HIDRELAY_CONFIG_DESCRIPTOR_BASE_LEN
        + (HIDRELAY_MAX_INTERFACE * HIDRELAY_HID_INTERFACE_DESCRIPTOR_LEN)
        + PLATFORM_USB_PORT_EXTRA_DESCRIPTOR_MAX_LEN
        + HIDRELAY_CDC_DESCRIPTOR_LEN] = {0};
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

static const uint8_t g_hid_report_desc_boot_keyboard[] = {
    0x05U,
    0x01U,
    0x09U,
    0x06U,
    0xA1U,
    0x01U,
    0x05U,
    0x07U,
    0x19U,
    0xE0U,
    0x29U,
    0xE7U,
    0x15U,
    0x00U,
    0x25U,
    0x01U,
    0x75U,
    0x01U,
    0x95U,
    0x08U,
    0x81U,
    0x02U,
    0x95U,
    0x01U,
    0x75U,
    0x08U,
    0x81U,
    0x01U,
    0x95U,
    0x05U,
    0x75U,
    0x01U,
    0x05U,
    0x08U,
    0x19U,
    0x01U,
    0x29U,
    0x05U,
    0x91U,
    0x02U,
    0x95U,
    0x01U,
    0x75U,
    0x03U,
    0x91U,
    0x01U,
    0x95U,
    0x06U,
    0x75U,
    0x08U,
    0x15U,
    0x00U,
    0x25U,
    0x65U,
    0x05U,
    0x07U,
    0x19U,
    0x00U,
    0x29U,
    0x65U,
    0x81U,
    0x00U,
    0xC0U
};

static const uint8_t g_hid_report_desc_boot_mouse[] = {
    0x05U,
    0x01U,
    0x09U,
    0x02U,
    0xA1U,
    0x01U,
    0x09U,
    0x01U,
    0xA1U,
    0x00U,
    0x05U,
    0x09U,
    0x19U,
    0x01U,
    0x29U,
    0x03U,
    0x15U,
    0x00U,
    0x25U,
    0x01U,
    0x95U,
    0x03U,
    0x75U,
    0x01U,
    0x81U,
    0x02U,
    0x95U,
    0x01U,
    0x75U,
    0x05U,
    0x81U,
    0x01U,
    0x05U,
    0x01U,
    0x09U,
    0x30U,
    0x09U,
    0x31U,
    0x09U,
    0x38U,
    0x15U,
    0x81U,
    0x25U,
    0x7FU,
    0x75U,
    0x08U,
    0x95U,
    0x03U,
    0x81U,
    0x06U,
    0xC0U,
    0xC0U
};

static const uint8_t * hidrelay_fallback_report_descriptor(
    hid_report_descriptor_source_t source,
    uint16_t * out_len
) {
    if (out_len == NULL) {
        return NULL;
    }

    if (source == HID_REPORT_DESCRIPTOR_SOURCE_FALLBACK_BOOT_KEYBOARD) {
        *out_len = (uint16_t)sizeof(g_hid_report_desc_boot_keyboard);
        return g_hid_report_desc_boot_keyboard;
    }

    if (source == HID_REPORT_DESCRIPTOR_SOURCE_FALLBACK_BOOT_MOUSE) {
        *out_len = (uint16_t)sizeof(g_hid_report_desc_boot_mouse);
        return g_hid_report_desc_boot_mouse;
    }

    *out_len = (uint16_t)sizeof(g_hid_report_desc_generic);
    return g_hid_report_desc_generic;
}

static uint8_t const * hidrelay_report_descriptor_for_interface(
    uint8_t instance,
    uint16_t * out_len
) {
    hid_report_policy_decision_t decision = {0};
    uint8_t protocol_mode = HID_TRANSPORT_PROTOCOL_UNKNOWN;
    uint16_t ignored_len = 0U;
    uint16_t * effective_len = out_len;
    const uint8_t * descriptor = NULL;

    if (effective_len == NULL) {
        effective_len = &ignored_len;
    }

    descriptor = transport_stack_usb_report_descriptor(instance, effective_len);
    protocol_mode = transport_stack_usb_protocol_mode(instance);
    hid_report_policy_decide(descriptor, *effective_len, protocol_mode, &decision);

    if (decision.source == HID_REPORT_DESCRIPTOR_SOURCE_NATIVE) {
        return descriptor;
    }

    return hidrelay_fallback_report_descriptor(decision.source, effective_len);
}

static void hidrelay_descriptor_put_u16(
    uint8_t * buffer,
    uint16_t value
) {
    if (buffer == NULL) {
        return;
    }

    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

#if defined(APP_HAS_DIAG_CDC)
static uint16_t hidrelay_append_cdc_descriptor(
    uint8_t * buffer,
    uint16_t offset,
    uint8_t cdc_control_interface_number
) {
    uint8_t cdc_data_interface_number = (uint8_t)(cdc_control_interface_number + 1U);

    if (buffer == NULL) {
        return offset;
    }

    buffer[offset++] = 8U;
    buffer[offset++] = TUSB_DESC_INTERFACE_ASSOCIATION;
    buffer[offset++] = cdc_control_interface_number;
    buffer[offset++] = HIDRELAY_CDC_INTERFACE_COUNT;
    buffer[offset++] = TUSB_CLASS_CDC;
    buffer[offset++] = CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL;
    buffer[offset++] = CDC_COMM_PROTOCOL_ATCOMMAND;
    buffer[offset++] = HIDRELAY_CDC_STRING_INDEX;

    buffer[offset++] = 9U;
    buffer[offset++] = TUSB_DESC_INTERFACE;
    buffer[offset++] = cdc_control_interface_number;
    buffer[offset++] = 0U;
    buffer[offset++] = 1U;
    buffer[offset++] = TUSB_CLASS_CDC;
    buffer[offset++] = CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL;
    buffer[offset++] = CDC_COMM_PROTOCOL_ATCOMMAND;
    buffer[offset++] = HIDRELAY_CDC_STRING_INDEX;

    buffer[offset++] = 5U;
    buffer[offset++] = TUSB_DESC_CS_INTERFACE;
    buffer[offset++] = CDC_FUNC_DESC_HEADER;
    hidrelay_descriptor_put_u16(&buffer[offset], 0x0120U);
    offset = (uint16_t)(offset + 2U);

    buffer[offset++] = 5U;
    buffer[offset++] = TUSB_DESC_CS_INTERFACE;
    buffer[offset++] = CDC_FUNC_DESC_CALL_MANAGEMENT;
    buffer[offset++] = 0x00U;
    buffer[offset++] = cdc_data_interface_number;

    buffer[offset++] = 4U;
    buffer[offset++] = TUSB_DESC_CS_INTERFACE;
    buffer[offset++] = CDC_FUNC_DESC_ABSTRACT_CONTROL_MANAGEMENT;
    buffer[offset++] = 0x02U;

    buffer[offset++] = 5U;
    buffer[offset++] = TUSB_DESC_CS_INTERFACE;
    buffer[offset++] = CDC_FUNC_DESC_UNION;
    buffer[offset++] = cdc_control_interface_number;
    buffer[offset++] = cdc_data_interface_number;

    buffer[offset++] = 7U;
    buffer[offset++] = TUSB_DESC_ENDPOINT;
    buffer[offset++] = HIDRELAY_CDC_EP_NOTIF;
    buffer[offset++] = TUSB_XFER_INTERRUPT;
    hidrelay_descriptor_put_u16(&buffer[offset], HIDRELAY_CDC_EP_NOTIF_SIZE);
    offset = (uint16_t)(offset + 2U);
    buffer[offset++] = HIDRELAY_CDC_EP_INTERVAL_MS;

    buffer[offset++] = 9U;
    buffer[offset++] = TUSB_DESC_INTERFACE;
    buffer[offset++] = cdc_data_interface_number;
    buffer[offset++] = 0U;
    buffer[offset++] = 2U;
    buffer[offset++] = TUSB_CLASS_CDC_DATA;
    buffer[offset++] = 0U;
    buffer[offset++] = 0U;
    buffer[offset++] = 0U;

    buffer[offset++] = 7U;
    buffer[offset++] = TUSB_DESC_ENDPOINT;
    buffer[offset++] = HIDRELAY_CDC_EP_OUT;
    buffer[offset++] = TUSB_XFER_BULK;
    hidrelay_descriptor_put_u16(&buffer[offset], HIDRELAY_CDC_EP_DATA_SIZE);
    offset = (uint16_t)(offset + 2U);
    buffer[offset++] = 0U;

    buffer[offset++] = 7U;
    buffer[offset++] = TUSB_DESC_ENDPOINT;
    buffer[offset++] = HIDRELAY_CDC_EP_IN;
    buffer[offset++] = TUSB_XFER_BULK;
    hidrelay_descriptor_put_u16(&buffer[offset], HIDRELAY_CDC_EP_DATA_SIZE);
    offset = (uint16_t)(offset + 2U);
    buffer[offset++] = 0U;

    return offset;
}
#else
static uint16_t hidrelay_append_cdc_descriptor(
    uint8_t * buffer,
    uint16_t offset,
    uint8_t cdc_control_interface_number
) {
    (void)buffer;
    (void)cdc_control_interface_number;
    return offset;
}
#endif

static uint16_t hidrelay_build_config_descriptor(uint8_t interface_count) {
    uint16_t offset = 0U;
    uint8_t index = 0U;
    uint8_t total_interface_count = 0U;
    uint16_t total_length = 0U;
    uint8_t extra_interface_count = platform_usb_port_extra_interface_count();
    uint16_t extra_descriptor_len = platform_usb_port_extra_descriptor_len();

    if (extra_descriptor_len > PLATFORM_USB_PORT_EXTRA_DESCRIPTOR_MAX_LEN) {
        extra_interface_count = 0U;
        extra_descriptor_len = 0U;
    }

    if (interface_count > HIDRELAY_MAX_INTERFACE) {
        interface_count = HIDRELAY_MAX_INTERFACE;
    }

    total_interface_count = (uint8_t)(interface_count + HIDRELAY_CDC_INTERFACE_COUNT);
    total_length = (uint16_t)(HIDRELAY_CONFIG_DESCRIPTOR_BASE_LEN
        + (interface_count * HIDRELAY_HID_INTERFACE_DESCRIPTOR_LEN)
        + extra_descriptor_len
        + HIDRELAY_CDC_DESCRIPTOR_LEN);

    total_interface_count = (uint8_t)(total_interface_count + extra_interface_count);

    g_config_desc[offset++] = 9U;
    g_config_desc[offset++] = TUSB_DESC_CONFIGURATION;
    hidrelay_descriptor_put_u16(&g_config_desc[offset], total_length);
    offset = (uint16_t)(offset + 2U);
    g_config_desc[offset++] = total_interface_count;
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

    if (extra_descriptor_len > 0U) {
        offset = platform_usb_port_append_extra_descriptor(g_config_desc, offset, interface_count);
    }
    offset = hidrelay_append_cdc_descriptor(
        g_config_desc,
        offset,
        (uint8_t)(interface_count + extra_interface_count)
    );

    return offset;
}

uint8_t const * tud_descriptor_device_cb(void) {
    /* The relay always presents its own identity. */
    return (const uint8_t *)&g_device_desc;
}

uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance) {
    uint16_t report_len = 0U;
    return hidrelay_report_descriptor_for_interface(instance, &report_len);
}

uint8_t const * tud_descriptor_configuration_cb(uint8_t index) {
    uint8_t interface_count = 0U;

    (void)index;
    interface_count = transport_stack_usb_interface_count();
    g_config_desc_len = hidrelay_build_config_descriptor(interface_count);
    (void)g_config_desc_len;
    return g_config_desc;
}

/*
 * Diagnostic override for the USB serial-number string: after a hang
 * recovery the boot report stuffs the frozen markers and the panic message
 * here, so the failure can be read from the host (ioreg / System
 * Information) without any tooling attached.
 */
static char g_hidrelay_diag_serial[HIDRELAY_STRING_LIMIT + 1U] = {0};

void usb_runtime_set_diag_serial(const char * text) {
    if (text == NULL) {
        g_hidrelay_diag_serial[0] = '\0';
        return;
    }
    (void)strncpy(g_hidrelay_diag_serial, text, HIDRELAY_STRING_LIMIT);
    g_hidrelay_diag_serial[HIDRELAY_STRING_LIMIT] = '\0';
}

uint16_t const * tud_descriptor_string_cb(
    uint8_t index,
    uint16_t langid
) {
    static uint16_t descriptor[HIDRELAY_STRING_LIMIT + 1U];
    static const char * const strings[] = {
        "HID Relay",
        "Hub",
        "00000001",
#if defined(APP_HAS_DIAG_CDC)
        "Diag CDC",
#endif
    };
    uint8_t char_count = 0U;
    const char * text = NULL;

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
    if ((index == 3U) && (g_hidrelay_diag_serial[0] != '\0')) {
        text = g_hidrelay_diag_serial;
    }
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

uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t * buffer,
    uint16_t reqlen
) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0U;
}

void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t const * buffer,
    uint16_t bufsize
) {
    (void)report_id;
    (void)report_type;

    transport_stack_ingest_usb_report(instance, buffer, bufsize);
}

#else

#include "usb_runtime.h"

void usb_runtime_set_diag_serial(const char * text) {
    (void)text;
}

#endif
