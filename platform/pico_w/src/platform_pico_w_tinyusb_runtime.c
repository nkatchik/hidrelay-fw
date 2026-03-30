#include "platform_pico_w_tinyusb_runtime.h"

#include <stddef.h>

#ifdef APP_PICO_HAS_TINYUSB
#include "tusb.h"
#endif

bool pico_w_tinyusb_runtime_init(void) {
#ifdef APP_PICO_HAS_TINYUSB
    return tusb_init();
#else
    return true;
#endif
}

void pico_w_tinyusb_runtime_poll(void) {
#ifdef APP_PICO_HAS_TINYUSB
    tud_task();
#endif
}

bool pico_w_tinyusb_runtime_send_in_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
) {
#ifdef APP_PICO_HAS_TINYUSB
    if ((report_len > 0U) && (report == NULL)) {
        return false;
    }

    if (report_len > UINT8_MAX) {
        return false;
    }

    if (!tud_hid_n_ready(interface_number)) {
        return false;
    }

    return tud_hid_n_report(interface_number, 0U, report, report_len);
#else
    (void)interface_number;
    (void)report;
    (void)report_len;
    return false;
#endif
}

bool pico_w_tinyusb_runtime_diag_write(
    const uint8_t * data,
    uint16_t data_len
) {
#if defined(APP_PICO_HAS_TINYUSB) && defined(APP_PICO_HAS_DIAG_CDC)
    uint16_t remaining = data_len;
    const uint8_t * cursor = data;

    if ((data_len > 0U) && (data == NULL)) {
        return false;
    }

    if (data_len == 0U) {
        return true;
    }

    if (!tud_ready() || !tud_cdc_n_connected(0U)) {
        return false;
    }

    while (remaining > 0U) {
        uint32_t available = tud_cdc_n_write_available(0U);
        uint32_t chunk = 0U;
        uint32_t written = 0U;

        if (available == 0U) {
            (void)tud_cdc_n_write_flush(0U);
            return false;
        }

        chunk = (remaining < available) ? remaining : available;
        written = tud_cdc_n_write(0U, cursor, chunk);

        if (written == 0U) {
            return false;
        }

        cursor += written;
        remaining = (uint16_t)(remaining - written);
    }

    (void)tud_cdc_n_write_flush(0U);
    return true;
#else
    (void)data;
    (void)data_len;
    return false;
#endif
}

#if defined(APP_PICO_HAS_TINYUSB) && defined(APP_PICO_HAS_DIAG_CDC)
void tud_cdc_rx_cb(uint8_t itf) {
    while (tud_cdc_n_available(itf) > 0U) {
        (void)tud_cdc_n_read_char(itf);
    }
}
#endif
