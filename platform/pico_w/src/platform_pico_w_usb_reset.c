#ifdef APP_HAS_TINYUSB

#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/usb_reset_interface.h"
#include "platform_pico_w_hw.h"
#include "platform_usb_port.h"
#include "tusb.h"

#undef TU_ATTR_WEAK
#define TU_ATTR_WEAK
#include "device/usbd_pvt.h"

static uint8_t g_reset_interface_number = 0U;
static uint8_t g_reset_pending_request = 0U;
static uint16_t g_reset_pending_bootsel_activity_mask = 0U;

enum {
    PICO_W_RESET_REQUEST_NONE = 0U,
    PICO_W_RESET_REQUEST_BOOTSEL = 1U,
    PICO_W_RESET_REQUEST_FLASH = 2U,
    PICO_W_RESET_DESCRIPTOR_LEN = 9U,
    PICO_W_RESET_STRING_INDEX = 0U,
};

/*
 * The picotool reset interface is exported as the platform's extra USB
 * interface, appended after the relay's HID interfaces.
 */
uint8_t platform_usb_port_extra_interface_count(void) {
    return 1U;
}

uint16_t platform_usb_port_extra_descriptor_len(void) {
    return PICO_W_RESET_DESCRIPTOR_LEN;
}

uint16_t platform_usb_port_append_extra_descriptor(
    uint8_t * buffer,
    uint16_t offset,
    uint8_t first_interface_number
) {
    if (buffer == NULL) {
        return offset;
    }

    buffer[offset++] = PICO_W_RESET_DESCRIPTOR_LEN;
    buffer[offset++] = TUSB_DESC_INTERFACE;
    buffer[offset++] = first_interface_number;
    buffer[offset++] = 0U;
    buffer[offset++] = 0U;
    buffer[offset++] = TUSB_CLASS_VENDOR_SPECIFIC;
    buffer[offset++] = RESET_INTERFACE_SUBCLASS;
    buffer[offset++] = RESET_INTERFACE_PROTOCOL;
    buffer[offset++] = PICO_W_RESET_STRING_INDEX;

    return offset;
}

static void pico_w_resetd_init(void) {
    g_reset_pending_request = PICO_W_RESET_REQUEST_NONE;
    g_reset_pending_bootsel_activity_mask = 0U;
}

static void pico_w_resetd_reset(uint8_t rhport) {
    (void)rhport;
    g_reset_interface_number = 0U;
    g_reset_pending_request = PICO_W_RESET_REQUEST_NONE;
    g_reset_pending_bootsel_activity_mask = 0U;
}

static uint16_t pico_w_resetd_open(
    uint8_t rhport,
    tusb_desc_interface_t const * itf_desc,
    uint16_t max_len
) {
    uint16_t const descriptor_len = sizeof(tusb_desc_interface_t);

    (void)rhport;

    TU_VERIFY(
        (itf_desc != NULL)
            && (itf_desc->bInterfaceClass == TUSB_CLASS_VENDOR_SPECIFIC)
            && (itf_desc->bInterfaceSubClass == RESET_INTERFACE_SUBCLASS)
            && (itf_desc->bInterfaceProtocol == RESET_INTERFACE_PROTOCOL),
        0U
    );
    TU_VERIFY(max_len >= descriptor_len, 0U);

    g_reset_interface_number = itf_desc->bInterfaceNumber;
    return descriptor_len;
}

static bool pico_w_resetd_control_xfer_cb(
    uint8_t rhport,
    uint8_t stage,
    tusb_control_request_t const * request
) {
    if (request == NULL) {
        return true;
    }

    if ((uint8_t)(request->wIndex & 0x00FFU) != g_reset_interface_number) {
        return false;
    }

    if (stage == CONTROL_STAGE_SETUP) {
        g_reset_pending_request = PICO_W_RESET_REQUEST_NONE;
        g_reset_pending_bootsel_activity_mask = 0U;

        if (request->bRequest == RESET_REQUEST_BOOTSEL) {
            g_reset_pending_request = PICO_W_RESET_REQUEST_BOOTSEL;
            g_reset_pending_bootsel_activity_mask = (uint16_t)(request->wValue & 0x007FU);
            if (!tud_control_status(rhport, request)) {
                g_reset_pending_request = PICO_W_RESET_REQUEST_NONE;
                g_reset_pending_bootsel_activity_mask = 0U;
                return false;
            }
            return true;
        }

        if (request->bRequest == RESET_REQUEST_FLASH) {
            g_reset_pending_request = PICO_W_RESET_REQUEST_FLASH;
            g_reset_pending_bootsel_activity_mask = 0U;
            if (!tud_control_status(rhport, request)) {
                g_reset_pending_request = PICO_W_RESET_REQUEST_NONE;
                return false;
            }
            return true;
        }

        return false;
    }

    if ((stage != CONTROL_STAGE_ACK) || (g_reset_pending_request == PICO_W_RESET_REQUEST_NONE)) {
        return true;
    }

    if ((g_reset_pending_request == PICO_W_RESET_REQUEST_BOOTSEL)
        && (request->bRequest == RESET_REQUEST_BOOTSEL)) {
        g_reset_pending_request = PICO_W_RESET_REQUEST_NONE;
        pico_w_hw_disarm_hang_report();
        reset_usb_boot(0U, (uint32_t)g_reset_pending_bootsel_activity_mask);
        return true;
    }

    if ((g_reset_pending_request == PICO_W_RESET_REQUEST_FLASH)
        && (request->bRequest == RESET_REQUEST_FLASH)) {
        g_reset_pending_request = PICO_W_RESET_REQUEST_NONE;
        pico_w_hw_disarm_hang_report();
        watchdog_reboot(0U, 0U, 0U);
        return true;
    }

    g_reset_pending_request = PICO_W_RESET_REQUEST_NONE;
    g_reset_pending_bootsel_activity_mask = 0U;
    return true;
}

static bool pico_w_resetd_xfer_cb(
    uint8_t rhport,
    uint8_t ep_addr,
    xfer_result_t result,
    uint32_t xferred_bytes
) {
    (void)rhport;
    (void)ep_addr;
    (void)result;
    (void)xferred_bytes;
    return true;
}

static usbd_class_driver_t const g_pico_w_resetd_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name = "RESET",
#endif
    .init = pico_w_resetd_init,
    .reset = pico_w_resetd_reset,
    .open = pico_w_resetd_open,
    .control_xfer_cb = pico_w_resetd_control_xfer_cb,
    .xfer_cb = pico_w_resetd_xfer_cb,
    .sof = NULL
};

usbd_class_driver_t const * usbd_app_driver_get_cb(uint8_t * driver_count) {
    if (driver_count != NULL) {
        *driver_count = 1U;
    }

    return &g_pico_w_resetd_driver;
}

#endif
