#ifdef APP_PICO_HAS_TINYUSB

#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/usb_reset_interface.h"
#include "tusb.h"

#undef TU_ATTR_WEAK
#define TU_ATTR_WEAK
#include "device/usbd_pvt.h"

static uint8_t g_reset_interface_number = 0U;

static void pico_w_resetd_init(void) {
}

static void pico_w_resetd_reset(uint8_t rhport) {
    (void)rhport;
    g_reset_interface_number = 0U;
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
    uint16_t interface_index = 0U;

    (void)rhport;

    if ((stage != CONTROL_STAGE_SETUP) || (request == NULL)) {
        return true;
    }

    interface_index = request->wIndex;
    if (interface_index != g_reset_interface_number) {
        return false;
    }

    if (request->bRequest == RESET_REQUEST_BOOTSEL) {
        reset_usb_boot(0U, (uint32_t)(request->wValue & 0x007FU));
        return true;
    }

    if (request->bRequest == RESET_REQUEST_FLASH) {
        watchdog_reboot(0U, 0U, 0U);
        return true;
    }

    return false;
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
