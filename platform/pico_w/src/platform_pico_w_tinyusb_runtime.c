#include "platform_pico_w_tinyusb_runtime.h"

#include <stddef.h>

#ifdef APP_PICO_HAS_TINYUSB
#include "pico/time.h"
#include "tusb.h"
#endif

#ifdef APP_PICO_HAS_TINYUSB
enum {
    PICO_W_TINYUSB_REENUM_DISCONNECT_MS = 120U,
};

static bool g_pico_w_tinyusb_reenum_pending = false;
static bool g_pico_w_tinyusb_reenum_disconnected = false;
static uint32_t g_pico_w_tinyusb_reenum_resume_ms = 0U;

static bool pico_w_tinyusb_runtime_time_reached(
    uint32_t now_ms,
    uint32_t target_ms
) {
    return (int32_t)(now_ms - target_ms) >= 0;
}

static void pico_w_tinyusb_runtime_reenumeration_tick(void) {
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (!g_pico_w_tinyusb_reenum_pending) {
        return;
    }

    if (!g_pico_w_tinyusb_reenum_disconnected) {
        if (!tud_connected() && !tud_mounted()) {
            g_pico_w_tinyusb_reenum_pending = false;
            return;
        }

        tud_disconnect();
        g_pico_w_tinyusb_reenum_disconnected = true;
        g_pico_w_tinyusb_reenum_resume_ms = now_ms + PICO_W_TINYUSB_REENUM_DISCONNECT_MS;
        return;
    }

    if (!pico_w_tinyusb_runtime_time_reached(now_ms, g_pico_w_tinyusb_reenum_resume_ms)) {
        return;
    }

    tud_connect();
    g_pico_w_tinyusb_reenum_pending = false;
    g_pico_w_tinyusb_reenum_disconnected = false;
    g_pico_w_tinyusb_reenum_resume_ms = 0U;
}
#endif

bool pico_w_tinyusb_runtime_init(void) {
#ifdef APP_PICO_HAS_TINYUSB
    g_pico_w_tinyusb_reenum_pending = false;
    g_pico_w_tinyusb_reenum_disconnected = false;
    g_pico_w_tinyusb_reenum_resume_ms = 0U;
    return tusb_init();
#else
    return true;
#endif
}

void pico_w_tinyusb_runtime_poll(void) {
#ifdef APP_PICO_HAS_TINYUSB
    tud_task();
    pico_w_tinyusb_runtime_reenumeration_tick();
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

void pico_w_tinyusb_runtime_request_reenumeration(void) {
#ifdef APP_PICO_HAS_TINYUSB
    g_pico_w_tinyusb_reenum_pending = true;
    g_pico_w_tinyusb_reenum_disconnected = false;
    g_pico_w_tinyusb_reenum_resume_ms = 0U;
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
