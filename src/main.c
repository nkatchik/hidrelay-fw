#include <stddef.h>
#include <string.h>

#include "app.h"
#include "platform_api.h"

enum {
    MAIN_PAIR_DB_SAVE_DEBOUNCE_MS = 2000U,
    MAIN_PAIR_DB_SAVE_MAX_STALE_MS = 15000U,
    MAIN_PAIR_DB_SAVE_RETRY_MS = 5000U,
};

static bool main_time_elapsed(
    uint32_t now_ms,
    uint32_t reference_ms,
    uint32_t duration_ms
) {
    return (int32_t)(now_ms - reference_ms) >= (int32_t)duration_ms;
}

int main(void) {
    app_t app = {0};
    pair_db_t initial_pair_db = {0};
    const bool has_initial_pair_db = platform_pair_db_load(&initial_pair_db);
    pair_db_t persisted_pair_db = {0};
    bool pair_db_save_pending = false;
    uint32_t pair_db_pending_since_ms = 0U;
    uint32_t pair_db_last_save_attempt_ms = 0U;
    app_output_t app_output = {
        .led_on = false,
        .pairing_active = false,
        .sleep_ms = 10U,
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
        .reconnect_request = {.valid = false, .device_id = {.bytes = {0}}},
        .forget_request = {.valid = false, .device_id = {.bytes = {0}}},
        .diag = {0},
        .usb_tx = {.valid = false, .interface_number = 0U, .report_len = 0U, .report = {0}},
        .bt_tx =
            {.valid = false, .hid_cid = 0U, .protocol_mode = 0U, .report_len = 0U, .report = {0}},
        .pair_db_dirty = false,
        .factory_reset_requested = false,
    };

    if (!platform_init()) {
        return 1;
    }

    app_init(&app, has_initial_pair_db ? &initial_pair_db : NULL);
    if (has_initial_pair_db) {
        persisted_pair_db = initial_pair_db;
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

        app_tick(&app, &app_input, &app_output);
        pair_db_changed = memcmp(&app.pair_db, &persisted_pair_db, sizeof(app.pair_db)) != 0;

        platform_output.led_on = app_output.led_on;
        platform_output.pairing_active = app_output.pairing_active;
        platform_output.sleep_ms = app_output.sleep_ms;
        platform_output.usb_interface_count = app_output.usb_interface_count;
        platform_output.usb_descriptor_generation = app_output.usb_descriptor_generation;
        (void)memcpy(
            platform_output.usb_interface_plan,
            app_output.usb_interface_plan,
            sizeof(platform_output.usb_interface_plan)
        );
        platform_output.reconnect_request = app_output.reconnect_request;
        platform_output.forget_request = app_output.forget_request;
        platform_output.diag = app_output.diag;
        platform_output.usb_tx = app_output.usb_tx;
        platform_output.bt_tx = app_output.bt_tx;
        platform_output.factory_reset_requested = app_output.factory_reset_requested;

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

                if (platform_pair_db_save(&app.pair_db)) {
                    persisted_pair_db = app.pair_db;
                    pair_db_save_pending = false;
                }
            }
        }
    }
}
