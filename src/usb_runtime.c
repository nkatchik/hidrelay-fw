#include "usb_runtime.h"

#include <stddef.h>
#include <string.h>

#include "hid_transport.h"

#ifdef APP_HAS_TINYUSB
#include "platform_api.h"
#include "tusb.h"

#if !defined(CFG_TUD_ENABLED) || (CFG_TUD_ENABLED != 1)
#error "TinyUSB device support requires CFG_TUD_ENABLED=1"
#endif

#if !defined(CFG_TUSB_RHPORT0_MODE) || ((CFG_TUSB_RHPORT0_MODE & OPT_MODE_DEVICE) == 0)
#error "TinyUSB rhport0 must be configured for device mode"
#endif
#endif

#ifdef APP_HAS_TINYUSB
enum {
    USB_RUNTIME_REENUM_DISCONNECT_MS = 120U,
    /*
     * After (re)attaching, give the host this long to finish enumerating
     * (reach the mounted state) before honoring another re-enumeration
     * request. Detaching mid-enumeration is what wedges the host's view of
     * the device; the timeout is the escape hatch when the host never mounts.
     */
    USB_RUNTIME_ENUM_GRACE_MS = 2000U,
    USB_RUNTIME_IN_REPORT_QUEUE_SIZE = 32U,
};

typedef struct {
    uint8_t interface_number;
    uint16_t report_len;
    uint8_t report[HID_TRANSPORT_REPORT_MAX_LEN];
} usb_runtime_in_report_t;

static bool g_usb_runtime_initialized = false;
static bool g_usb_runtime_reenum_pending = false;
static bool g_usb_runtime_reenum_disconnected = false;
static uint32_t g_usb_runtime_reenum_resume_ms = 0U;
static uint32_t g_usb_runtime_attach_ms = 0U;
static uint32_t g_usb_runtime_descriptor_activity_count = 0U;
static usb_runtime_in_report_t g_usb_runtime_in_report_queue[USB_RUNTIME_IN_REPORT_QUEUE_SIZE] = {
    0
};
static uint8_t g_usb_runtime_in_report_queue_head = 0U;
static uint8_t g_usb_runtime_in_report_queue_tail = 0U;
static uint8_t g_usb_runtime_in_report_queue_count = 0U;

static void usb_runtime_clear_in_report_queue(void) {
    (void)memset(g_usb_runtime_in_report_queue, 0, sizeof(g_usb_runtime_in_report_queue));
    g_usb_runtime_in_report_queue_head = 0U;
    g_usb_runtime_in_report_queue_tail = 0U;
    g_usb_runtime_in_report_queue_count = 0U;
}

static void usb_runtime_pop_in_report_queue(void) {
    (void)memset(
        &g_usb_runtime_in_report_queue[g_usb_runtime_in_report_queue_head],
        0,
        sizeof(g_usb_runtime_in_report_queue[g_usb_runtime_in_report_queue_head])
    );
    g_usb_runtime_in_report_queue_head =
        (uint8_t)((g_usb_runtime_in_report_queue_head + 1U) % USB_RUNTIME_IN_REPORT_QUEUE_SIZE);
    g_usb_runtime_in_report_queue_count = (uint8_t)(g_usb_runtime_in_report_queue_count - 1U);
}

static bool usb_runtime_time_reached(
    uint32_t now_ms,
    uint32_t target_ms
) {
    return (int32_t)(now_ms - target_ms) >= 0;
}

static void usb_runtime_reenumeration_tick(void) {
    const uint32_t now_ms = platform_uptime_ms();

    if (!g_usb_runtime_reenum_pending) {
        return;
    }

    if (!g_usb_runtime_reenum_disconnected) {
        if (!tud_connected() && !tud_mounted()) {
            g_usb_runtime_reenum_pending = false;
            return;
        }

        /*
         * The host is still enumerating the device (attached but not yet
         * mounted): let that finish before detaching again, or the host can be
         * left with a half-enumerated, unresponsive view of the device. The
         * pending request is not lost -- it runs on mount or after the grace
         * timeout.
         */
        if (!tud_mounted()
            && !usb_runtime_time_reached(
                now_ms,
                g_usb_runtime_attach_ms + USB_RUNTIME_ENUM_GRACE_MS
            )) {
            return;
        }

        tud_disconnect();
        g_usb_runtime_reenum_disconnected = true;
        g_usb_runtime_reenum_resume_ms = now_ms + USB_RUNTIME_REENUM_DISCONNECT_MS;
        return;
    }

    if (!usb_runtime_time_reached(now_ms, g_usb_runtime_reenum_resume_ms)) {
        return;
    }

    tud_connect();
    g_usb_runtime_attach_ms = now_ms;
    g_usb_runtime_reenum_pending = false;
    g_usb_runtime_reenum_disconnected = false;
    g_usb_runtime_reenum_resume_ms = 0U;
}

static bool usb_runtime_try_send_in_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
) {
    if (!g_usb_runtime_initialized || !tud_mounted() || !tud_ready()) {
        return false;
    }

    if (!tud_hid_n_ready(interface_number)) {
        return false;
    }

    return tud_hid_n_report(interface_number, 0U, report, report_len);
}

static bool usb_runtime_queue_in_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
) {
    usb_runtime_in_report_t * queued_report = NULL;

    if ((report_len > 0U) && (report == NULL)) {
        return false;
    }

    if (report_len > HID_TRANSPORT_REPORT_MAX_LEN) {
        return false;
    }

    if (g_usb_runtime_in_report_queue_count >= USB_RUNTIME_IN_REPORT_QUEUE_SIZE) {
        return false;
    }

    queued_report = &g_usb_runtime_in_report_queue[g_usb_runtime_in_report_queue_tail];
    (void)memset(queued_report, 0, sizeof(*queued_report));
    queued_report->interface_number = interface_number;
    queued_report->report_len = report_len;
    if (report_len > 0U) {
        (void)memcpy(queued_report->report, report, report_len);
    }

    g_usb_runtime_in_report_queue_tail =
        (uint8_t)((g_usb_runtime_in_report_queue_tail + 1U) % USB_RUNTIME_IN_REPORT_QUEUE_SIZE);
    g_usb_runtime_in_report_queue_count = (uint8_t)(g_usb_runtime_in_report_queue_count + 1U);
    return true;
}

static void usb_runtime_drain_in_report_queue(void) {
    if (!g_usb_runtime_initialized || !tud_mounted()) {
        return;
    }

    if (!tud_ready()) {
        return;
    }

    while (g_usb_runtime_in_report_queue_count > 0U) {
        usb_runtime_in_report_t * queued_report =
            &g_usb_runtime_in_report_queue[g_usb_runtime_in_report_queue_head];

        if (!usb_runtime_try_send_in_report(
                queued_report->interface_number,
                queued_report->report,
                queued_report->report_len
            )) {
            return;
        }

        usb_runtime_pop_in_report_queue();
    }
}
#endif

bool usb_runtime_init(void) {
#ifdef APP_HAS_TINYUSB
    g_usb_runtime_initialized = false;
    g_usb_runtime_reenum_pending = false;
    g_usb_runtime_reenum_disconnected = false;
    g_usb_runtime_reenum_resume_ms = 0U;
    g_usb_runtime_descriptor_activity_count = 0U;
    usb_runtime_clear_in_report_queue();
    g_usb_runtime_initialized = tusb_init();
    /* Boot-time attach starts the host's first enumeration of the device. */
    g_usb_runtime_attach_ms = platform_uptime_ms();
    return g_usb_runtime_initialized;
#else
    return true;
#endif
}

bool usb_runtime_is_initialized(void) {
#ifdef APP_HAS_TINYUSB
    return g_usb_runtime_initialized;
#else
    return false;
#endif
}

void usb_runtime_mark_descriptor_activity(void) {
#ifdef APP_HAS_TINYUSB
    g_usb_runtime_descriptor_activity_count = g_usb_runtime_descriptor_activity_count + 1U;
#endif
}

uint32_t usb_runtime_descriptor_activity_count(void) {
#ifdef APP_HAS_TINYUSB
    return g_usb_runtime_descriptor_activity_count;
#else
    return 0U;
#endif
}

void usb_runtime_poll(void) {
#ifdef APP_HAS_TINYUSB
    if (!g_usb_runtime_initialized) {
        g_usb_runtime_initialized = tusb_init();
        if (!g_usb_runtime_initialized) {
            return;
        }
    }

    tud_task();
    usb_runtime_reenumeration_tick();
    usb_runtime_drain_in_report_queue();
#endif
}

bool usb_runtime_send_in_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
) {
#ifdef APP_HAS_TINYUSB
    if ((report_len > 0U) && (report == NULL)) {
        return false;
    }

    if (report_len > HID_TRANSPORT_REPORT_MAX_LEN) {
        return false;
    }

    if (!g_usb_runtime_initialized || !tud_mounted() || !tud_ready()) {
        return false;
    }

    if ((g_usb_runtime_in_report_queue_count == 0U)
        && usb_runtime_try_send_in_report(interface_number, report, report_len)) {
        return true;
    }

    return usb_runtime_queue_in_report(interface_number, report, report_len);
#else
    (void)interface_number;
    (void)report;
    (void)report_len;
    return false;
#endif
}

void usb_runtime_request_reenumeration(void) {
#ifdef APP_HAS_TINYUSB
    if (g_usb_runtime_reenum_pending) {
        /*
         * A cycle is already pending: the host reads descriptors only after the
         * upcoming re-attach, so any state change landing before then is picked
         * up by that same cycle -- nothing to record.
         */
        return;
    }

    usb_runtime_clear_in_report_queue();
    g_usb_runtime_reenum_pending = true;
    g_usb_runtime_reenum_disconnected = false;
    g_usb_runtime_reenum_resume_ms = 0U;
#endif
}

bool usb_runtime_diag_write(
    const uint8_t * data,
    uint16_t data_len
) {
#if defined(APP_HAS_TINYUSB) && defined(APP_HAS_DIAG_CDC)
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
