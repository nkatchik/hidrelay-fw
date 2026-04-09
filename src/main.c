#include <stddef.h>
#include <string.h>

#include "app.h"
#include "platform_api.h"

enum {
    MAIN_PAIR_DB_SAVE_DEBOUNCE_MS = 2000U,
    MAIN_PAIR_DB_SAVE_MAX_STALE_MS = 15000U,
    MAIN_PAIR_DB_SAVE_RETRY_MS = 5000U,
};

/*
 * RP2040 default stack is small; keep large app state out of main() frame to
 * avoid early boot stack overflow.
 */
static app_t g_app = {0};
static pair_db_t g_initial_pair_db = {0};
static pair_db_t g_persisted_pair_db = {0};
static app_output_t g_app_output = {
    .led_on = false,
    .pairing_active = false,
    .sleep_us = 100U,
    .usb_interface_count = 0U,
    .usb_descriptor_generation = 0U,
    .usb_tx_queue_depth = 0U,
    .bt_tx_queue_depth = 0U,
    .usb_tx_queue_high_watermark = 0U,
    .bt_tx_queue_high_watermark = 0U,
    .usb_tx_dropped = 0U,
    .bt_tx_dropped = 0U,
    .active_device_count = 0U,
    .usb_interface_plan = {0},
    .reconnect_request =
        {.valid = false,
            .device_id = {.bytes = {0}},
            .bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN,
            .bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_UNKNOWN},
    .forget_request = {.valid = false, .device_id = {.bytes = {0}}},
    .diag = {0},
    .usb_tx = {.valid = false, .interface_number = 0U, .report_len = 0U, .report = {0}},
    .bt_tx =
        {.valid = false,
            .hid_cid = 0U,
            .bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN,
            .protocol_mode = 0U,
            .report_len = 0U,
            .report = {0}},
    .pair_db_dirty = false,
    .factory_reset_requested = false,
};

static bool main_time_elapsed(
    uint32_t now_ms,
    uint32_t reference_ms,
    uint32_t duration_ms
) {
    return (int32_t)(now_ms - reference_ms) >= (int32_t)duration_ms;
}

int main(void) {
    const bool has_initial_pair_db = platform_pair_db_load(&g_initial_pair_db);
    bool pair_db_save_pending = false;
    uint32_t pair_db_pending_since_ms = 0U;
    uint32_t pair_db_last_save_attempt_ms = 0U;

    if (!platform_init()) {
        return 1;
    }

    app_init(&g_app, has_initial_pair_db ? &g_initial_pair_db : NULL);
    if (has_initial_pair_db) {
        g_persisted_pair_db = g_initial_pair_db;
    }

    for (;;) {
        platform_input_t platform_input = {0};
        platform_output_t platform_output = {0};
        app_input_t app_input = {0};
        bool pair_db_changed = false;

        platform_poll(&platform_input);

        app_input.button_pressed = platform_input.button_pressed;
        app_input.now_ms = platform_input.uptime_ms;
        app_input.transport_event = platform_input.transport_event;

        app_tick(&g_app, &app_input, &g_app_output);
        pair_db_changed = memcmp(&g_app.pair_db, &g_persisted_pair_db, sizeof(g_app.pair_db)) != 0;

        platform_output.led_on = g_app_output.led_on;
        platform_output.pairing_active = g_app_output.pairing_active;
        platform_output.sleep_us = g_app_output.sleep_us;
        platform_output.usb_interface_count = g_app_output.usb_interface_count;
        platform_output.usb_descriptor_generation = g_app_output.usb_descriptor_generation;
        (void)memcpy(
            platform_output.usb_interface_plan,
            g_app_output.usb_interface_plan,
            sizeof(platform_output.usb_interface_plan)
        );
        platform_output.reconnect_request = g_app_output.reconnect_request;
        platform_output.forget_request = g_app_output.forget_request;
        platform_output.diag = g_app_output.diag;
        platform_output.usb_tx = g_app_output.usb_tx;
        platform_output.bt_tx = g_app_output.bt_tx;
        platform_output.factory_reset_requested = g_app_output.factory_reset_requested;

        platform_apply(&platform_output);

        if (pair_db_changed && !pair_db_save_pending) {
            pair_db_save_pending = true;
            pair_db_pending_since_ms = platform_input.uptime_ms;
        }

        if (!pair_db_changed) {
            pair_db_save_pending = false;
        }

        if (pair_db_save_pending) {
            const bool debounce_elapsed = main_time_elapsed(
                platform_input.uptime_ms,
                pair_db_pending_since_ms,
                MAIN_PAIR_DB_SAVE_DEBOUNCE_MS
            );
            const bool max_stale_elapsed = main_time_elapsed(
                platform_input.uptime_ms,
                pair_db_pending_since_ms,
                MAIN_PAIR_DB_SAVE_MAX_STALE_MS
            );
            const bool retry_elapsed = main_time_elapsed(
                                           platform_input.uptime_ms,
                                           pair_db_last_save_attempt_ms,
                                           MAIN_PAIR_DB_SAVE_RETRY_MS
                                       )
                || (pair_db_last_save_attempt_ms == 0U);

            if ((debounce_elapsed || max_stale_elapsed) && retry_elapsed) {
                pair_db_last_save_attempt_ms = platform_input.uptime_ms;

                if (platform_pair_db_save(&g_app.pair_db)) {
                    g_persisted_pair_db = g_app.pair_db;
                    pair_db_save_pending = false;
                }
            }
        }
    }
}
