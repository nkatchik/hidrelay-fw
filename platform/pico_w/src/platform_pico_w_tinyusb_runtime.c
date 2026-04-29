#include "platform_pico_w_tinyusb_runtime.h"

#include <stddef.h>
#include <string.h>

#include "hid_transport.h"

#ifdef APP_PICO_HAS_TINYUSB
#include "pico/time.h"
#include "tusb.h"

#if !defined(CFG_TUD_ENABLED) || (CFG_TUD_ENABLED != 1)
#error "TinyUSB device support requires CFG_TUD_ENABLED=1"
#endif

#if !defined(CFG_TUSB_RHPORT0_MODE) || ((CFG_TUSB_RHPORT0_MODE & OPT_MODE_DEVICE) == 0)
#error "TinyUSB rhport0 must be configured for device mode"
#endif
#endif

#ifdef APP_PICO_HAS_TINYUSB
enum {
    PICO_W_TINYUSB_REENUM_DISCONNECT_MS = 120U,
    PICO_W_TINYUSB_IN_REPORT_QUEUE_SIZE = 32U,
};

typedef struct {
    uint8_t interface_number;
    uint16_t report_len;
    uint8_t report[HID_TRANSPORT_REPORT_MAX_LEN];
} pico_w_tinyusb_in_report_t;

static bool g_pico_w_tinyusb_initialized = false;
static bool g_pico_w_tinyusb_reenum_pending = false;
static bool g_pico_w_tinyusb_reenum_disconnected = false;
static uint32_t g_pico_w_tinyusb_reenum_resume_ms = 0U;
static uint32_t g_pico_w_tinyusb_descriptor_activity_count = 0U;
static pico_w_tinyusb_in_report_t
    g_pico_w_tinyusb_in_report_queue[PICO_W_TINYUSB_IN_REPORT_QUEUE_SIZE] = {0};
static uint8_t g_pico_w_tinyusb_in_report_queue_head = 0U;
static uint8_t g_pico_w_tinyusb_in_report_queue_tail = 0U;
static uint8_t g_pico_w_tinyusb_in_report_queue_count = 0U;

static void pico_w_tinyusb_runtime_clear_in_report_queue(void) {
    (void)memset(g_pico_w_tinyusb_in_report_queue, 0, sizeof(g_pico_w_tinyusb_in_report_queue));
    g_pico_w_tinyusb_in_report_queue_head = 0U;
    g_pico_w_tinyusb_in_report_queue_tail = 0U;
    g_pico_w_tinyusb_in_report_queue_count = 0U;
}

static void pico_w_tinyusb_runtime_pop_in_report_queue(void) {
    (void)memset(
        &g_pico_w_tinyusb_in_report_queue[g_pico_w_tinyusb_in_report_queue_head],
        0,
        sizeof(g_pico_w_tinyusb_in_report_queue[g_pico_w_tinyusb_in_report_queue_head])
    );
    g_pico_w_tinyusb_in_report_queue_head = (uint8_t)((g_pico_w_tinyusb_in_report_queue_head + 1U)
        % PICO_W_TINYUSB_IN_REPORT_QUEUE_SIZE);
    g_pico_w_tinyusb_in_report_queue_count = (uint8_t)(g_pico_w_tinyusb_in_report_queue_count - 1U);
}

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

static bool pico_w_tinyusb_runtime_try_send_in_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
) {
    if (!g_pico_w_tinyusb_initialized || !tud_mounted() || !tud_ready()) {
        return false;
    }

    if (!tud_hid_n_ready(interface_number)) {
        return false;
    }

    return tud_hid_n_report(interface_number, 0U, report, report_len);
}

static bool pico_w_tinyusb_runtime_queue_in_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
) {
    pico_w_tinyusb_in_report_t * queued_report = NULL;

    if ((report_len > 0U) && (report == NULL)) {
        return false;
    }

    if (report_len > HID_TRANSPORT_REPORT_MAX_LEN) {
        return false;
    }

    if (g_pico_w_tinyusb_in_report_queue_count >= PICO_W_TINYUSB_IN_REPORT_QUEUE_SIZE) {
        return false;
    }

    queued_report = &g_pico_w_tinyusb_in_report_queue[g_pico_w_tinyusb_in_report_queue_tail];
    (void)memset(queued_report, 0, sizeof(*queued_report));
    queued_report->interface_number = interface_number;
    queued_report->report_len = report_len;
    if (report_len > 0U) {
        (void)memcpy(queued_report->report, report, report_len);
    }

    g_pico_w_tinyusb_in_report_queue_tail = (uint8_t)((g_pico_w_tinyusb_in_report_queue_tail + 1U)
        % PICO_W_TINYUSB_IN_REPORT_QUEUE_SIZE);
    g_pico_w_tinyusb_in_report_queue_count = (uint8_t)(g_pico_w_tinyusb_in_report_queue_count + 1U);
    return true;
}

static void pico_w_tinyusb_runtime_drain_in_report_queue(void) {
    if (!g_pico_w_tinyusb_initialized || !tud_mounted()) {
        return;
    }

    if (!tud_ready()) {
        return;
    }

    while (g_pico_w_tinyusb_in_report_queue_count > 0U) {
        pico_w_tinyusb_in_report_t * queued_report =
            &g_pico_w_tinyusb_in_report_queue[g_pico_w_tinyusb_in_report_queue_head];

        if (!pico_w_tinyusb_runtime_try_send_in_report(
                queued_report->interface_number,
                queued_report->report,
                queued_report->report_len
            )) {
            return;
        }

        pico_w_tinyusb_runtime_pop_in_report_queue();
    }
}
#endif

bool pico_w_tinyusb_runtime_init(void) {
#ifdef APP_PICO_HAS_TINYUSB
    g_pico_w_tinyusb_initialized = false;
    g_pico_w_tinyusb_reenum_pending = false;
    g_pico_w_tinyusb_reenum_disconnected = false;
    g_pico_w_tinyusb_reenum_resume_ms = 0U;
    g_pico_w_tinyusb_descriptor_activity_count = 0U;
    pico_w_tinyusb_runtime_clear_in_report_queue();
    g_pico_w_tinyusb_initialized = tusb_init();
    return g_pico_w_tinyusb_initialized;
#else
    return true;
#endif
}

bool pico_w_tinyusb_runtime_is_initialized(void) {
#ifdef APP_PICO_HAS_TINYUSB
    return g_pico_w_tinyusb_initialized;
#else
    return false;
#endif
}

void pico_w_tinyusb_runtime_mark_descriptor_activity(void) {
#ifdef APP_PICO_HAS_TINYUSB
    g_pico_w_tinyusb_descriptor_activity_count = g_pico_w_tinyusb_descriptor_activity_count + 1U;
#endif
}

uint32_t pico_w_tinyusb_runtime_descriptor_activity_count(void) {
#ifdef APP_PICO_HAS_TINYUSB
    return g_pico_w_tinyusb_descriptor_activity_count;
#else
    return 0U;
#endif
}

void pico_w_tinyusb_runtime_poll(void) {
#ifdef APP_PICO_HAS_TINYUSB
    if (!g_pico_w_tinyusb_initialized) {
        g_pico_w_tinyusb_initialized = tusb_init();
        if (!g_pico_w_tinyusb_initialized) {
            return;
        }
    }

    tud_task();
    pico_w_tinyusb_runtime_reenumeration_tick();
    pico_w_tinyusb_runtime_drain_in_report_queue();
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

    if (report_len > HID_TRANSPORT_REPORT_MAX_LEN) {
        return false;
    }

    if (!g_pico_w_tinyusb_initialized || !tud_mounted() || !tud_ready()) {
        return false;
    }

    if ((g_pico_w_tinyusb_in_report_queue_count == 0U)
        && pico_w_tinyusb_runtime_try_send_in_report(interface_number, report, report_len)) {
        return true;
    }

    return pico_w_tinyusb_runtime_queue_in_report(interface_number, report, report_len);
#else
    (void)interface_number;
    (void)report;
    (void)report_len;
    return false;
#endif
}

void pico_w_tinyusb_runtime_request_reenumeration(void) {
#ifdef APP_PICO_HAS_TINYUSB
    if (g_pico_w_tinyusb_reenum_pending) {
        return;
    }

    pico_w_tinyusb_runtime_clear_in_report_queue();
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
