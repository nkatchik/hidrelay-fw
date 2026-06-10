#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "app_diag.h"
#include "pair_store.h"
#include "platform_api.h"
#include "transport_stack.h"
#include "usb_runtime.h"

enum {
    MAIN_PAIR_DB_SAVE_DEBOUNCE_MS = 2000U,
    MAIN_PAIR_DB_SAVE_MAX_STALE_MS = 15000U,
    MAIN_PAIR_DB_SAVE_RETRY_MS = 5000U,
    MAIN_DIAG_EMIT_INTERVAL_MS = 50U,
    /*
     * Flash writes stall the core with IRQs masked for tens to hundreds of
     * milliseconds, during which nothing drains the radio's 4 KB
     * radio-to-host buffer. A device streaming input (a trackpad in use)
     * can overflow it in that blackout, and the SDK's shared-bus driver
     * panics on overflow ("cyw43 buffer overflow") -- the
     * trackpad-connection freeze. Saves therefore wait for a quiet window
     * with no recent Bluetooth input; the hard cap keeps persistence from
     * being deferred forever under continuous streaming.
     */
    MAIN_PAIR_DB_SAVE_BT_QUIET_MS = 400U,
    MAIN_PAIR_DB_SAVE_QUIET_WAIT_CAP_MS = 30000U,
};

/* Main-thread hang-checkpoint markers (see platform_hang_checkpoint_main).
 * Markers 7-8 are written from inside transport_stack_poll (lock wait /
 * USB poll). */
enum {
    MAIN_HANG_MARKER_POLL = 1U,
    MAIN_HANG_MARKER_POLL_DONE = 2U,
    MAIN_HANG_MARKER_APP_TICK = 3U,
    MAIN_HANG_MARKER_APPLY_OUTPUT = 4U,
    MAIN_HANG_MARKER_PAIR_STORE_SAVE = 5U,
    MAIN_HANG_MARKER_LOOP_DONE = 6U,
    /* 7-8 written from transport_stack_poll. */
    MAIN_HANG_MARKER_APPLY_TRANSPORT = 9U,
    MAIN_HANG_MARKER_APPLY_LED = 10U,
    MAIN_HANG_MARKER_APPLY_SLEEP = 11U
};

enum {
    MAIN_HANG_BLINK_ON_MS = 350U,
    MAIN_HANG_BLINK_OFF_MS = 350U,
    MAIN_HANG_BLINK_GROUP_GAP_MS = 1500U
};

static void main_hang_blink_delay_ms(uint32_t delay_ms) {
    uint32_t elapsed = 0U;

    for (elapsed = 0U; elapsed < delay_ms; elapsed += 10U) {
        platform_sleep_us(10U * 1000U);
    }
}

static void main_hang_blink_group(uint8_t blink_count) {
    uint8_t blink = 0U;

    for (blink = 0U; blink < blink_count; blink++) {
        platform_set_led(true);
        main_hang_blink_delay_ms(MAIN_HANG_BLINK_ON_MS);
        platform_set_led(false);
        main_hang_blink_delay_ms(MAIN_HANG_BLINK_OFF_MS);
    }
}

/*
 * After a watchdog recovery from a firmware hang, blink three groups before
 * normal startup: Bluetooth-context marker + 1, main-thread marker + 1, and
 * panic class + 1 (so zero values still produce one blink). The panic group
 * separates "an SDK panic fired" (and which one) from a pure loop or
 * deadlock. Localizes the wedge without any tooling.
 */
static void main_report_hang_if_any(void) {
    static char diag_serial[40] = {0};
    uint8_t bt_marker = 0U;
    uint8_t main_marker = 0U;
    uint8_t panic_class = 0U;
    const char * panic_text = NULL;

    if (!platform_take_hang_report(&bt_marker, &main_marker, &panic_class, &panic_text)) {
        return;
    }

    /*
     * Publish the report as the USB serial-number string too: the panic
     * message itself is readable host-side (ioreg / System Information)
     * where blink groups can only carry numbers.
     */
    (void)snprintf(
        diag_serial,
        sizeof(diag_serial),
        "B%u M%u %s",
        (unsigned)bt_marker,
        (unsigned)main_marker,
        (panic_text != NULL) ? panic_text : "no-panic"
    );
    usb_runtime_set_diag_serial(diag_serial);

    main_hang_blink_delay_ms(MAIN_HANG_BLINK_GROUP_GAP_MS);
    main_hang_blink_group((uint8_t)(bt_marker + 1U));
    main_hang_blink_delay_ms(MAIN_HANG_BLINK_GROUP_GAP_MS);
    main_hang_blink_group((uint8_t)(main_marker + 1U));
    main_hang_blink_delay_ms(MAIN_HANG_BLINK_GROUP_GAP_MS);
    main_hang_blink_group((uint8_t)(panic_class + 1U));
    main_hang_blink_delay_ms(MAIN_HANG_BLINK_GROUP_GAP_MS);
}

/*
 * MCU default stacks are small; keep large app state out of main() frame to
 * avoid early boot stack overflow.
 */
static app_t g_app = {0};
static pair_db_t g_initial_pair_db = {0};
static pair_db_t g_persisted_pair_db = {0};
static uint8_t g_diag_frame[APP_DIAG_FRAME_MAX_LEN] = {0};
static hid_transport_usb_tx_t g_pending_usb_tx =
    {.valid = false, .interface_number = 0U, .report_len = 0U, .report = {0}};
static hid_transport_bt_tx_t g_pending_bt_tx = {
    .valid = false,
    .hid_cid = 0U,
    .bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN,
    .protocol_mode = 0U,
    .report_len = 0U,
    .report = {0}
};
static app_output_t g_app_output = {
    .led_on = false,
    .pairing_active = false,
    .pairing_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN,
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
    .factory_reset_requested = false,
};
static uint32_t g_applied_usb_descriptor_generation = 0U;

static bool main_time_elapsed(
    uint32_t now_ms,
    uint32_t reference_ms,
    uint32_t duration_ms
) {
    return (int32_t)(now_ms - reference_ms) >= (int32_t)duration_ms;
}

static void main_merge_transport_diag(
    hid_transport_diag_snapshot_t * diag,
    const transport_stack_state_t * transport_state
) {
    if ((diag == NULL) || (transport_state == NULL)) {
        return;
    }

    diag->stack_event_depth = transport_state->event_queue_depth;
    diag->stack_event_high_watermark = transport_state->event_queue_high_watermark;
    diag->stack_event_dropped = transport_state->event_queue_dropped;
    diag->stack_connect_pending = transport_state->connect_pending;
    diag->stack_reconnect_pending = transport_state->reconnect_pending;
    diag->stack_connect_mode = transport_state->connect_mode;
    diag->stack_reconnect_attempt_index = transport_state->reconnect_attempt_index;
    diag->stack_reconnect_attempt_count = transport_state->reconnect_attempt_count;
    diag->stack_last_connect_status = transport_state->last_connect_status;
}

static void main_publish_diag(const hid_transport_diag_snapshot_t * diag) {
    /*
     * Emitting a diag frame every main-loop iteration (~kHz) floods the CDC and
     * starves tud_task() during the USB re-enumeration that runs when a HID
     * device attaches -- enough to wedge enumeration on this hardware. Throttle
     * the encode+write to a modest rate; the snapshot is still published to the
     * app every loop so no state transition is missed.
     */
    static uint32_t last_emit_ms = 0U;
    static bool last_emit_valid = false;
    uint16_t frame_len = 0U;
    uint32_t now_ms = 0U;

    if (diag == NULL) {
        return;
    }

    app_diag_publish(diag);

    now_ms = platform_uptime_ms();
    if (last_emit_valid && ((uint32_t)(now_ms - last_emit_ms) < MAIN_DIAG_EMIT_INTERVAL_MS)) {
        return;
    }
    last_emit_ms = now_ms;
    last_emit_valid = true;

    frame_len = app_diag_encode_frame(diag, g_diag_frame, sizeof(g_diag_frame));
    if (frame_len > 0U) {
        (void)usb_runtime_diag_write(g_diag_frame, frame_len);
    }
}

static bool main_send_usb_tx(const hid_transport_usb_tx_t * tx) {
    if ((tx == NULL) || !tx->valid) {
        return true;
    }

    return transport_stack_send_usb_report(tx->interface_number, tx->report, tx->report_len);
}

static bool main_send_bt_tx(const hid_transport_bt_tx_t * tx) {
    if ((tx == NULL) || !tx->valid) {
        return true;
    }

    return transport_stack_send_bt_report(
        tx->hid_cid,
        tx->bt_link_type,
        tx->protocol_mode,
        tx->report,
        tx->report_len
    );
}

static void main_clear_pending_usb_tx(void) {
    (void)memset(&g_pending_usb_tx, 0, sizeof(g_pending_usb_tx));
}

static void main_clear_pending_bt_tx(void) {
    (void)memset(&g_pending_bt_tx, 0, sizeof(g_pending_bt_tx));
}

static void main_clear_pending_transport_tx(void) {
    main_clear_pending_usb_tx();
    main_clear_pending_bt_tx();
}

static void main_flush_pending_transport_tx(void) {
    if (g_pending_usb_tx.valid && main_send_usb_tx(&g_pending_usb_tx)) {
        main_clear_pending_usb_tx();
    }

    if (g_pending_bt_tx.valid && main_send_bt_tx(&g_pending_bt_tx)) {
        main_clear_pending_bt_tx();
    }
}

static bool main_transport_tx_blocked(void) {
    return g_pending_usb_tx.valid || g_pending_bt_tx.valid;
}

static void main_accept_output_transport_tx(const app_output_t * output) {
    if (output == NULL) {
        return;
    }

    if (output->usb_tx.valid) {
        if (!g_pending_usb_tx.valid && !main_send_usb_tx(&output->usb_tx)) {
            g_pending_usb_tx = output->usb_tx;
        }
    }

    if (output->bt_tx.valid) {
        if (!g_pending_bt_tx.valid && !main_send_bt_tx(&output->bt_tx)) {
            g_pending_bt_tx = output->bt_tx;
        }
    }
}

static void main_run_factory_reset_sequence(void) {
    /* Keep reset ordering in app/main so reboot can be instrumented centrally. */
    (void)platform_factory_reset_erase_persistent_data();
    platform_reboot();
}

static void main_apply_output(const app_output_t * output) {
    transport_stack_state_t transport_state = {0};
    bool have_transport_state = false;
    hid_transport_diag_snapshot_t diag = {0};
    uint32_t sleep_us = 0U;

    if (output == NULL) {
        return;
    }

    platform_hang_checkpoint_main(MAIN_HANG_MARKER_APPLY_TRANSPORT);
    have_transport_state = transport_stack_state_get(&transport_state);
    sleep_us = output->sleep_us;

    if (output->forget_request.valid) {
        (void)transport_stack_forget_device(&output->forget_request.device_id);
    }

    if (output->usb_descriptor_generation != g_applied_usb_descriptor_generation) {
        main_clear_pending_transport_tx();
        g_applied_usb_descriptor_generation = output->usb_descriptor_generation;
    }

    transport_stack_set_usb_plan(
        output->usb_interface_count,
        output->usb_descriptor_generation,
        output->usb_interface_plan
    );
    transport_stack_set_pairing(output->pairing_active, output->pairing_link_type);

    if (output->reconnect_request.valid) {
        (void)transport_stack_request_reconnect(
            &output->reconnect_request.device_id,
            output->reconnect_request.bt_link_type,
            output->reconnect_request.bt_addr_type
        );
    }

    main_accept_output_transport_tx(output);

    if (output->factory_reset_requested) {
        main_run_factory_reset_sequence();
        return;
    }

    diag = output->diag;
    if (have_transport_state) {
        main_merge_transport_diag(&diag, &transport_state);
        if (transport_state.event_queue_depth > 0U) {
            sleep_us = 0U;
        }
    }
    if (main_transport_tx_blocked()) {
        sleep_us = 0U;
    }
    main_publish_diag(&diag);

    /* The LED is a radio-chip GPIO: this write is a bus transaction to the
     * same chip carrying Bluetooth, so it gets its own hang marker. */
    platform_hang_checkpoint_main(MAIN_HANG_MARKER_APPLY_LED);
    platform_set_led(output->led_on);
    platform_hang_checkpoint_main(MAIN_HANG_MARKER_APPLY_SLEEP);
#if defined(APP_HAS_TINYUSB)
    if (sleep_us > USB_RUNTIME_MAX_POLL_SLEEP_US) {
        sleep_us = USB_RUNTIME_MAX_POLL_SLEEP_US;
    }
#endif
    platform_sleep_us(sleep_us);
}

int main(void) {
#if defined(APP_DEBUG_WIPE_ALL_ON_BOOT) && APP_DEBUG_WIPE_ALL_ON_BOOT
    /*
     * Debug-only hard wipe: erase all persisted Pair DB + BTstack security
     * material on every boot before attempting any load.
     */
    (void)platform_factory_reset_erase_persistent_data();
#endif
    const bool has_initial_pair_db = pair_store_load(&g_initial_pair_db);
    bool pair_db_save_pending = false;
    uint32_t pair_db_pending_since_ms = 0U;
    uint32_t pair_db_last_save_attempt_ms = 0U;

    if (!platform_init()) {
        return 1;
    }

    main_report_hang_if_any();

    if (!transport_stack_init()) {
        return 1;
    }

    platform_watchdog_enable();

    app_diag_init();
    app_init(&g_app, has_initial_pair_db ? &g_initial_pair_db : NULL);
    if (has_initial_pair_db) {
        g_persisted_pair_db = g_initial_pair_db;
    }

    uint32_t last_bt_report_ms = 0U;
    bool bt_report_seen = false;

    for (;;) {
        app_input_t app_input = {0};
        bool pair_db_changed = false;

        platform_watchdog_feed();
        app_input.button_pressed = platform_button_pressed();
        app_input.now_ms = platform_uptime_ms();
        app_input.transport_event.type = HID_TRANSPORT_EVENT_NONE;
        platform_hang_checkpoint_main(MAIN_HANG_MARKER_POLL);
        transport_stack_poll(app_input.now_ms);
        platform_hang_checkpoint_main(MAIN_HANG_MARKER_POLL_DONE);
        main_flush_pending_transport_tx();
        app_input.usb_tx_blocked = g_pending_usb_tx.valid;
        app_input.bt_tx_blocked = g_pending_bt_tx.valid;

        if (!transport_stack_take_event(&app_input.transport_event)) {
            app_input.transport_event.type = HID_TRANSPORT_EVENT_NONE;
        }

        if (app_input.transport_event.type == HID_TRANSPORT_EVENT_BT_HID_REPORT) {
            last_bt_report_ms = app_input.now_ms;
            bt_report_seen = true;
        }

        platform_hang_checkpoint_main(MAIN_HANG_MARKER_APP_TICK);
        app_tick(&g_app, &app_input, &g_app_output);
        pair_db_changed = memcmp(&g_app.pair_db, &g_persisted_pair_db, sizeof(g_app.pair_db)) != 0;

        platform_hang_checkpoint_main(MAIN_HANG_MARKER_APPLY_OUTPUT);
        main_apply_output(&g_app_output);

        if (pair_db_changed && !pair_db_save_pending) {
            pair_db_save_pending = true;
            pair_db_pending_since_ms = app_input.now_ms;
        }

        if (!pair_db_changed) {
            pair_db_save_pending = false;
        }

        if (pair_db_save_pending) {
            const bool debounce_elapsed = main_time_elapsed(
                app_input.now_ms,
                pair_db_pending_since_ms,
                MAIN_PAIR_DB_SAVE_DEBOUNCE_MS
            );
            const bool max_stale_elapsed = main_time_elapsed(
                app_input.now_ms,
                pair_db_pending_since_ms,
                MAIN_PAIR_DB_SAVE_MAX_STALE_MS
            );
            const bool retry_elapsed = main_time_elapsed(
                                           app_input.now_ms,
                                           pair_db_last_save_attempt_ms,
                                           MAIN_PAIR_DB_SAVE_RETRY_MS
                                       )
                || (pair_db_last_save_attempt_ms == 0U);
            const bool bt_quiet = !bt_report_seen
                || main_time_elapsed(
                    app_input.now_ms,
                    last_bt_report_ms,
                    MAIN_PAIR_DB_SAVE_BT_QUIET_MS
                );
            const bool quiet_wait_capped = main_time_elapsed(
                app_input.now_ms,
                pair_db_pending_since_ms,
                MAIN_PAIR_DB_SAVE_QUIET_WAIT_CAP_MS
            );

            if ((debounce_elapsed || max_stale_elapsed)
                && retry_elapsed
                && (bt_quiet || quiet_wait_capped)) {
                pair_db_last_save_attempt_ms = app_input.now_ms;

                platform_hang_checkpoint_main(MAIN_HANG_MARKER_PAIR_STORE_SAVE);
                if (pair_store_save(&g_app.pair_db)) {
                    g_persisted_pair_db = g_app.pair_db;
                    pair_db_save_pending = false;
                }
            }
        }

        platform_hang_checkpoint_main(MAIN_HANG_MARKER_LOOP_DONE);
    }
}
