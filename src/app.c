#include "app.h"

#include <stddef.h>
#include <string.h>

enum {
    APP_IDLE_SLEEP_US = 5000U,
    APP_CONNECTED_SLEEP_US = 5U,
    APP_REMOVE_LAST_MAX_AGE_MS = 60U * 60U * 1000U,
    APP_RECONNECT_TIMEOUT_MS = 30000U,
    APP_RECONNECT_BASE_BACKOFF_MS = 500U,
    APP_RECONNECT_MAX_BACKOFF_SHIFT = 2U,
    APP_RECONNECT_STACK_REJECT_RETRY_MS = 1000U,
    APP_RECONNECT_STACK_NOT_READY_RETRY_MS = 3000U,
    APP_REMOVE_LAST_BLINK_COUNT = 1U,
    APP_FACTORY_RESET_BLINK_COUNT = 3U,
    APP_PAIRING_ERROR_BLINK_CONNECT_COUNT = 1U,
    APP_PAIRING_ERROR_BLINK_AUTH_COUNT = 2U,
    APP_PAIRING_ERROR_BLINK_STACK_COUNT = 3U,
    APP_PAIRING_ERROR_BLINK_UNKNOWN_COUNT = 4U,
    APP_PAIRING_ERROR_BLINK_CLASSIC_CONNECT_COUNT = 5U,
    APP_PAIRING_ERROR_BLINK_LE_CONNECT_COUNT = 6U,
    APP_PAIRING_ERROR_BLINK_LE_HIDS_COUNT = 7U
};

static led_ui_state_t app_led_state_from_bt_state(bt_manager_state_t bt_state) {
    if (bt_state == BT_MANAGER_STATE_PAIRING) {
        return LED_UI_STATE_PAIRING;
    }

    if (bt_state == BT_MANAGER_STATE_ACTIVE) {
        return LED_UI_STATE_CONNECTED;
    }

    if (bt_state == BT_MANAGER_STATE_ERROR) {
        return LED_UI_STATE_ERROR;
    }

    return LED_UI_STATE_IDLE;
}

static uint8_t app_pairing_error_blink_count(uint8_t reconnect_result) {
    static const struct {
        uint8_t reconnect_result;
        uint8_t blink_count;
    } error_codes[] = {
        {HID_TRANSPORT_RECONNECT_RESULT_CONNECT_FAILED, APP_PAIRING_ERROR_BLINK_CONNECT_COUNT},
        {HID_TRANSPORT_RECONNECT_RESULT_TIMEOUT, APP_PAIRING_ERROR_BLINK_CONNECT_COUNT},
        {HID_TRANSPORT_RECONNECT_RESULT_AUTH_FAILED, APP_PAIRING_ERROR_BLINK_AUTH_COUNT},
        {HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED, APP_PAIRING_ERROR_BLINK_STACK_COUNT},
        {HID_TRANSPORT_RECONNECT_RESULT_PAIRING_CLASSIC_CONNECT_FAILED,
            APP_PAIRING_ERROR_BLINK_CLASSIC_CONNECT_COUNT},
        {HID_TRANSPORT_RECONNECT_RESULT_PAIRING_LE_CONNECT_FAILED,
            APP_PAIRING_ERROR_BLINK_LE_CONNECT_COUNT},
        {HID_TRANSPORT_RECONNECT_RESULT_PAIRING_LE_HIDS_FAILED,
            APP_PAIRING_ERROR_BLINK_LE_HIDS_COUNT},
    };
    size_t index = 0U;

    for (index = 0U; index < (sizeof(error_codes) / sizeof(error_codes[0])); index++) {
        if (error_codes[index].reconnect_result == reconnect_result) {
            return error_codes[index].blink_count;
        }
    }

    if ((reconnect_result != HID_TRANSPORT_RECONNECT_RESULT_NONE)
        && (reconnect_result != HID_TRANSPORT_RECONNECT_RESULT_REQUESTED)
        && (reconnect_result != HID_TRANSPORT_RECONNECT_RESULT_SUCCESS)) {
        return APP_PAIRING_ERROR_BLINK_UNKNOWN_COUNT;
    }

    return 0U;
}

static void app_seed_pair_db(
    pair_db_t * dst,
    const pair_db_t * src
) {
    if ((dst == NULL) || (src == NULL)) {
        return;
    }

    *dst = *src;

    if (dst->count > PAIR_DB_MAX_DEVICE) {
        dst->count = PAIR_DB_MAX_DEVICE;
    }
}

static bool app_device_id_equal(
    const pair_device_id_t * lhs,
    const pair_device_id_t * rhs
) {
    if ((lhs == NULL) || (rhs == NULL)) {
        return false;
    }

    return memcmp(lhs->bytes, rhs->bytes, sizeof(lhs->bytes)) == 0;
}

static bool app_device_active(
    const bt_manager_t * manager,
    const pair_device_id_t * device_id
) {
    uint8_t index = 0U;

    if ((manager == NULL) || (device_id == NULL)) {
        return false;
    }

    for (index = 0U; index < bt_manager_active_count(manager); index++) {
        bt_hid_device_t active_device = {0};

        if (!bt_manager_active_get(manager, index, &active_device)) {
            continue;
        }

        if (app_device_id_equal(&active_device.device_id, device_id)) {
            return true;
        }
    }

    return false;
}

static bool app_get_reconnect_candidate(
    const app_t * app,
    uint32_t now_ms,
    pair_db_entry_t * out_entry
) {
    uint8_t index = 0U;
    bool found = false;
    pair_db_entry_t best = {0};

    if ((app == NULL) || (out_entry == NULL)) {
        return false;
    }

    for (index = 0U; index < app->pair_db.count; index++) {
        const pair_db_entry_t * entry = &app->pair_db.entries[index];

        if (app_device_active(&app->bt_manager, &entry->device_id)) {
            continue;
        }

        /*
         * Classic HID devices own their reconnection: a bonded keyboard pages
         * the host itself on power-on, wake, and link loss, and the relay is
         * always page-scannable to accept it. Relay-initiated paging would only
         * collide with the device's own page (two simultaneous connection
         * attempts to/from one address), which delays or breaks the reconnect.
         * The relay initiates only for LE, where the central must connect.
         */
        if (entry->last_bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_CLASSIC) {
            continue;
        }

        if ((entry->reconnect_allowed == 0U)
            || ((int32_t)(now_ms - entry->reconnect_retry_after_ms) < 0)) {
            continue;
        }

        if (!found
            || (entry->reconnect_fail_count < best.reconnect_fail_count)
            || ((entry->reconnect_fail_count == best.reconnect_fail_count)
                && (entry->last_seen_ms > best.last_seen_ms))) {
            best = *entry;
            found = true;
        }
    }

    if (!found) {
        return false;
    }

    *out_entry = best;
    return true;
}

static uint32_t app_reconnect_backoff_ms(uint8_t fail_count) {
    uint8_t shift = 0U;

    if (fail_count > 0U) {
        shift = (uint8_t)(fail_count - 1U);
    }

    if (shift > APP_RECONNECT_MAX_BACKOFF_SHIFT) {
        shift = APP_RECONNECT_MAX_BACKOFF_SHIFT;
    }

    return APP_RECONNECT_BASE_BACKOFF_MS << shift;
}

static bool app_reconnect_result_is_connect_failure(uint8_t reconnect_result) {
    return (reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_TIMEOUT)
        || (reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_CONNECT_FAILED)
        || (reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_PAIRING_CLASSIC_CONNECT_FAILED)
        || (reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_PAIRING_LE_CONNECT_FAILED)
        || (reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_PAIRING_LE_HIDS_FAILED);
}

static void app_reconnect_clear_inflight(app_t * app) {
    if (app == NULL) {
        return;
    }

    app->reconnect_inflight = false;
    app->reconnect_started_ms = 0U;
    (void)memset(&app->reconnect_device_id, 0, sizeof(app->reconnect_device_id));
}

static void app_reconnect_mark_success(
    app_t * app,
    const pair_device_id_t * device_id,
    uint32_t now_ms
) {
    if ((app == NULL) || (device_id == NULL)) {
        return;
    }

    (void)pair_db_mark_reconnect_success(&app->pair_db, device_id, now_ms);
    app->reconnect_success_count = app->reconnect_success_count + 1U;
    app->reconnect_last_result = HID_TRANSPORT_RECONNECT_RESULT_SUCCESS;
    app->reconnect_last_status_code = 0U;
    app_reconnect_clear_inflight(app);
}

static void app_reconnect_note_success_pending_open(app_t * app) {
    if (app == NULL) {
        return;
    }

    app->reconnect_last_result = HID_TRANSPORT_RECONNECT_RESULT_SUCCESS;
    app->reconnect_last_status_code = 0U;
}

static void app_reconnect_mark_failure(
    app_t * app,
    uint8_t reconnect_result,
    uint8_t status_code,
    uint32_t now_ms
) {
    uint8_t index = 0U;
    pair_db_entry_t entry = {0};
    uint8_t fail_count = 0U;
    uint32_t retry_after_ms = now_ms;

    if ((app == NULL)
        || !app->reconnect_inflight
        || (reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_NONE)
        || (reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_REQUESTED)) {
        return;
    }

    if (!pair_db_find(&app->pair_db, &app->reconnect_device_id, &index)
        || !pair_db_get_entry(&app->pair_db, index, &entry)) {
        app_reconnect_clear_inflight(app);
        return;
    }

    fail_count = entry.reconnect_fail_count;

    if (app_reconnect_result_is_connect_failure(reconnect_result)) {
        if (fail_count < UINT8_MAX) {
            fail_count = (uint8_t)(fail_count + 1U);
        }

        /*
         * Transient connect/timeouts are expected for sleeping peripherals.
         * Keep retrying with bounded backoff instead of entering lockout.
         */
        retry_after_ms = now_ms + app_reconnect_backoff_ms(fail_count);
    } else if (reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_AUTH_FAILED) {
        if (fail_count < UINT8_MAX) {
            fail_count = (uint8_t)(fail_count + 1U);
        }

        /*
         * Some keyboards can surface transient auth/key-missing statuses after
         * long idle windows. Keep retrying with bounded backoff so reconnect
         * recovers without requiring a device reboot.
         */
        retry_after_ms = now_ms + app_reconnect_backoff_ms(fail_count);
    } else if (reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED) {
        retry_after_ms = now_ms
            + ((status_code == 2U) ? APP_RECONNECT_STACK_NOT_READY_RETRY_MS
                                   : APP_RECONNECT_STACK_REJECT_RETRY_MS);
    }

    (void)pair_db_mark_reconnect_failure(
        &app->pair_db,
        &app->reconnect_device_id,
        fail_count,
        retry_after_ms
    );

    app->reconnect_failure_count = app->reconnect_failure_count + 1U;
    app->reconnect_last_result = reconnect_result;
    app->reconnect_last_status_code = status_code;
    app_reconnect_clear_inflight(app);
}

static bool app_handle_transport_event(
    app_t * app,
    const app_input_t * input,
    bool * out_open_ingested
) {
    const hid_transport_event_t * event = NULL;
    bool close_ingested = false;

    if (out_open_ingested != NULL) {
        *out_open_ingested = false;
    }

    if ((app == NULL) || (input == NULL)) {
        return false;
    }

    event = &input->transport_event;

    switch (event->type) {
        case HID_TRANSPORT_EVENT_BT_HID_OPEN: {
            const bool open_ingested = bt_manager_ingest_hid_open(
                &app->bt_manager,
                &event->device_id,
                event->hid_cid,
                event->bt_link_type,
                event->bt_addr_type,
                event->vendor_id,
                event->product_id,
                event->report_descriptor_len,
                input->now_ms
            );

            if (out_open_ingested != NULL) {
                *out_open_ingested = open_ingested;
            }
        } break;
        case HID_TRANSPORT_EVENT_BT_HID_CLOSE: {
            if (event->hid_cid != 0U) {
                close_ingested = bt_manager_ingest_hid_close(
                    &app->bt_manager,
                    event->hid_cid,
                    event->bt_link_type,
                    input->now_ms
                );
            }

            if (!close_ingested) {
                close_ingested = bt_manager_ingest_hid_close_device(
                    &app->bt_manager,
                    &event->device_id,
                    event->bt_link_type,
                    input->now_ms
                );
            }
        } break;
        case HID_TRANSPORT_EVENT_BT_HID_DESCRIPTOR:
            (void)bt_manager_ingest_hid_descriptor(
                &app->bt_manager,
                event->hid_cid,
                event->bt_link_type,
                event->report_descriptor_len,
                input->now_ms
            );
            break;
        case HID_TRANSPORT_EVENT_BT_HID_PROTOCOL:
            (void)bt_manager_ingest_hid_protocol(
                &app->bt_manager,
                event->hid_cid,
                event->bt_link_type,
                event->protocol_mode,
                input->now_ms
            );
            break;
        case HID_TRANSPORT_EVENT_BT_HID_REPORT:
            (void)usb_bridge_ingest_bt_report(
                &app->usb_bridge,
                event->hid_cid,
                event->bt_link_type,
                event->report,
                event->report_len
            );
            break;
        case HID_TRANSPORT_EVENT_USB_HID_REPORT:
            (void)usb_bridge_ingest_usb_report(
                &app->usb_bridge,
                event->interface_number,
                event->report,
                event->report_len
            );
            break;
        case HID_TRANSPORT_EVENT_RECONNECT_RESULT:
        case HID_TRANSPORT_EVENT_NONE:
        default:
            break;
    }

    return close_ingested;
}

void app_init(
    app_t * app,
    const pair_db_t * initial_pair_db
) {
    if (app == NULL) {
        return;
    }

    button_fsm_init(&app->button_fsm);
    led_ui_init(&app->led_ui);
    pair_db_init(&app->pair_db);
    app_seed_pair_db(&app->pair_db, initial_pair_db);
    (void)pair_db_prepare_runtime(&app->pair_db, 0U);
    bt_manager_init(&app->bt_manager, &app->pair_db);
    usb_bridge_init(&app->usb_bridge);
    app->reconnect_inflight = false;
    (void)memset(&app->reconnect_device_id, 0, sizeof(app->reconnect_device_id));
    app->reconnect_started_ms = 0U;
    app->reconnect_attempt_count = 0U;
    app->reconnect_success_count = 0U;
    app->reconnect_failure_count = 0U;
    app->reconnect_last_result = HID_TRANSPORT_RECONNECT_RESULT_NONE;
    app->reconnect_last_status_code = 0U;
    app->factory_reset_armed = false;
    app->startup_cue_pending = true;
}

void app_tick(
    app_t * app,
    const app_input_t * input,
    app_output_t * output
) {
    button_command_t command = BUTTON_COMMAND_NONE;
    hid_transport_forget_request_t forget_request = {.valid = false, .device_id = {.bytes = {0}}};
    usb_bridge_telemetry_t bridge_telemetry = {0};
    bt_manager_state_t bt_state = BT_MANAGER_STATE_IDLE;
    bt_manager_state_t bt_state_before_event = BT_MANAGER_STATE_IDLE;
    pair_db_entry_t reconnect_candidate = {0};
    uint8_t pairing_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
    uint8_t reconnect_index = 0U;
    uint8_t interface_index = 0U;
    bool disconnect_event_cue = false;
    bool open_event_ingested = false;
    bool pairing_attempt_started = false;
    bool pairing_mode_command = false;
    bool pairing_success_cue = false;
    uint8_t pairing_attempt_error_blinks = 0U;

    if ((app == NULL) || (input == NULL) || (output == NULL)) {
        return;
    }

    if (app->startup_cue_pending) {
        led_ui_trigger_startup_cue(&app->led_ui, input->now_ms);
        app->startup_cue_pending = false;
    }

    command = button_fsm_update(&app->button_fsm, input->button_pressed, input->now_ms);

    if (command == BUTTON_COMMAND_PAIR_BLE) {
        pairing_mode_command = bt_manager_start_pairing(
            &app->bt_manager,
            input->now_ms,
            HID_TRANSPORT_BT_LINK_TYPE_LE
        );
    } else if (command == BUTTON_COMMAND_PAIR_CLASSIC) {
        pairing_mode_command = bt_manager_start_pairing(
            &app->bt_manager,
            input->now_ms,
            HID_TRANSPORT_BT_LINK_TYPE_CLASSIC
        );
    } else if (command == BUTTON_COMMAND_SINGLE_CLICK) {
        (void)bt_manager_cancel_pair_any(&app->bt_manager);
    } else if (command == BUTTON_COMMAND_REMOVE_LAST) {
        pair_device_id_t removed_device_id = {0};
        bool removed_device_known = false;

        if (pair_db_count(&app->pair_db) > 0U) {
            const uint8_t last_index = (uint8_t)(pair_db_count(&app->pair_db) - 1U);
            removed_device_known = pair_db_get(&app->pair_db, last_index, &removed_device_id);
        }

        if (bt_manager_remove_last_if_recent(
                &app->bt_manager,
                input->now_ms,
                APP_REMOVE_LAST_MAX_AGE_MS
            )) {
            led_ui_trigger_long_blink(&app->led_ui, APP_REMOVE_LAST_BLINK_COUNT, input->now_ms);

            if (removed_device_known) {
                forget_request.valid = true;
                forget_request.device_id = removed_device_id;
            }
        }
    } else if (command == BUTTON_COMMAND_REMOVE_ALL) {
        (void)bt_manager_remove_all(&app->bt_manager);
        led_ui_set_state(
            &app->led_ui,
            app_led_state_from_bt_state(bt_manager_state(&app->bt_manager)),
            input->now_ms
        );
        led_ui_trigger_long_blink(&app->led_ui, APP_FACTORY_RESET_BLINK_COUNT, input->now_ms);
        app->factory_reset_armed = true;
    }

    bt_state_before_event = bt_manager_state(&app->bt_manager);
    if ((input->transport_event.type == HID_TRANSPORT_EVENT_RECONNECT_RESULT)
        && (bt_state_before_event == BT_MANAGER_STATE_PAIRING)) {
        if (input->transport_event.reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_REQUESTED) {
            pairing_attempt_started = true;
        } else {
            pairing_attempt_error_blinks =
                app_pairing_error_blink_count(input->transport_event.reconnect_result);
        }
    }

    disconnect_event_cue = app_handle_transport_event(app, input, &open_event_ingested);
    (void)pair_db_reconnect_recover_expired(&app->pair_db, input->now_ms);
    if (pairing_attempt_error_blinks > 0U) {
        (void)bt_manager_cancel_pair_any(&app->bt_manager);
    }

    if (app->reconnect_inflight
        && (input->transport_event.type == HID_TRANSPORT_EVENT_BT_HID_OPEN)
        && app_device_id_equal(&input->transport_event.device_id, &app->reconnect_device_id)) {
        app_reconnect_mark_success(app, &input->transport_event.device_id, input->now_ms);
    }

    if (app->reconnect_inflight
        && (input->transport_event.type == HID_TRANSPORT_EVENT_RECONNECT_RESULT)
        && app_device_id_equal(&input->transport_event.device_id, &app->reconnect_device_id)) {
        if (input->transport_event.reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_SUCCESS) {
            if (app_device_active(&app->bt_manager, &input->transport_event.device_id)) {
                app_reconnect_mark_success(app, &input->transport_event.device_id, input->now_ms);
            } else {
                app_reconnect_note_success_pending_open(app);
            }
        } else {
            app_reconnect_mark_failure(
                app,
                input->transport_event.reconnect_result,
                input->transport_event.status_code,
                input->now_ms
            );
        }
    }

    bt_manager_tick(&app->bt_manager, input->now_ms);
    /*
     * The full bridge sync copies and compares every interface slot. The
     * manager's active set only changes via transport events or button
     * commands, so on quiescent ticks (the common case at the 5us connected
     * poll rate) skip it unless a warm grace window still needs time-based
     * processing.
     */
    if ((input->transport_event.type != HID_TRANSPORT_EVENT_NONE)
        || (command != BUTTON_COMMAND_NONE)
        || usb_bridge_sync_pending(&app->usb_bridge)) {
        usb_bridge_sync_from_bt_manager(&app->usb_bridge, &app->bt_manager, input->now_ms);
    }
    usb_bridge_tick(&app->usb_bridge, input->now_ms);

    bt_state = bt_manager_state(&app->bt_manager);
    pairing_link_type = bt_manager_pairing_link_type(&app->bt_manager);
    pairing_success_cue = (bt_state_before_event == BT_MANAGER_STATE_PAIRING)
        && (bt_state == BT_MANAGER_STATE_ACTIVE)
        && (input->transport_event.type == HID_TRANSPORT_EVENT_BT_HID_OPEN)
        && app->led_ui.pairing_attempt_active;
    if (pairing_attempt_error_blinks > 0U) {
        led_ui_trigger_error_blink(&app->led_ui, pairing_attempt_error_blinks, input->now_ms);
    }
    if (pairing_mode_command) {
        led_ui_set_pairing_attempt_active(&app->led_ui, false, input->now_ms);
    }
    led_ui_set_pairing_classic_mode(
        &app->led_ui,
        (bt_state == BT_MANAGER_STATE_PAIRING)
            && (pairing_link_type == HID_TRANSPORT_BT_LINK_TYPE_CLASSIC),
        input->now_ms
    );
    led_ui_set_state(&app->led_ui, app_led_state_from_bt_state(bt_state), input->now_ms);
    if (pairing_success_cue) {
        led_ui_trigger_connected_cue(&app->led_ui, input->now_ms);
    }
    /*
     * A device (re)connecting while others stay connected leaves the manager
     * in ACTIVE, so the IDLE->CONNECTED transition that normally produces the
     * connected cue never happens. Trigger it explicitly for that case --
     * symmetric to the disconnect cue below, which is also event-driven.
     */
    if (open_event_ingested
        && (bt_state_before_event == BT_MANAGER_STATE_ACTIVE)
        && (bt_state == BT_MANAGER_STATE_ACTIVE)) {
        led_ui_trigger_connected_cue(&app->led_ui, input->now_ms);
    }
    if (disconnect_event_cue
        && (bt_state_before_event != BT_MANAGER_STATE_PAIRING)
        && (bt_state != BT_MANAGER_STATE_PAIRING)) {
        led_ui_trigger_disconnect_cue(&app->led_ui, input->now_ms);
    }
    if (bt_state == BT_MANAGER_STATE_PAIRING) {
        if (pairing_attempt_started) {
            led_ui_set_pairing_attempt_active(&app->led_ui, true, input->now_ms);
        }
    }

    output->led_on = led_ui_tick(&app->led_ui, input->now_ms);
    output->pairing_active = bt_state == BT_MANAGER_STATE_PAIRING;
    output->pairing_link_type = pairing_link_type;
    output->usb_interface_count = usb_bridge_interface_count(&app->usb_bridge);
    output->usb_descriptor_generation = usb_bridge_descriptor_generation(&app->usb_bridge);
    output->active_device_count = bt_manager_active_count(&app->bt_manager);
    output->sleep_us =
        (output->active_device_count > 0U) ? APP_CONNECTED_SLEEP_US : APP_IDLE_SLEEP_US;
    (void)memset(output->usb_interface_plan, 0, sizeof(output->usb_interface_plan));

    for (interface_index = 0U; (interface_index < output->usb_interface_count)
        && (interface_index < HID_TRANSPORT_MAX_INTERFACE);
        interface_index++) {
        usb_bridge_interface_t interface_info = {0};

        if (!usb_bridge_interface_get(&app->usb_bridge, interface_index, &interface_info)) {
            continue;
        }

        output->usb_interface_plan[interface_index].report_descriptor_len =
            interface_info.report_descriptor_len;
        output->usb_interface_plan[interface_index].protocol_mode = interface_info.protocol_mode;
        output->usb_interface_plan[interface_index].hid_cid = interface_info.hid_cid;
        output->usb_interface_plan[interface_index].bt_link_type = interface_info.bt_link_type;
        output->usb_interface_plan[interface_index].device_id = interface_info.device_id;
    }

    output->reconnect_request.valid = false;
    output->forget_request.valid = false;
    output->factory_reset_requested = false;
    output->reconnect_request.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
    output->reconnect_request.bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_UNKNOWN;
    (void)memset(
        &output->reconnect_request.device_id,
        0,
        sizeof(output->reconnect_request.device_id)
    );
    (void)memset(&output->forget_request.device_id, 0, sizeof(output->forget_request.device_id));

    if (app->reconnect_inflight
        && !pair_db_find(&app->pair_db, &app->reconnect_device_id, &reconnect_index)) {
        app_reconnect_clear_inflight(app);
    }

    if (bt_state != BT_MANAGER_STATE_PAIRING) {
        if (app->reconnect_inflight
            && ((input->now_ms - app->reconnect_started_ms) >= APP_RECONNECT_TIMEOUT_MS)) {
            app_reconnect_mark_failure(
                app,
                HID_TRANSPORT_RECONNECT_RESULT_TIMEOUT,
                0U,
                input->now_ms
            );
        }

        if ((output->active_device_count < BT_MANAGER_MAX_ACTIVE_DEVICE)
            && !app->reconnect_inflight
            && app_get_reconnect_candidate(app, input->now_ms, &reconnect_candidate)) {
            output->reconnect_request.valid = true;
            output->reconnect_request.device_id = reconnect_candidate.device_id;
            output->reconnect_request.bt_link_type = reconnect_candidate.last_bt_link_type;
            output->reconnect_request.bt_addr_type = reconnect_candidate.last_bt_addr_type;
            app->reconnect_inflight = true;
            app->reconnect_device_id = reconnect_candidate.device_id;
            app->reconnect_started_ms = input->now_ms;
            app->reconnect_attempt_count = app->reconnect_attempt_count + 1U;
            app->reconnect_last_result = HID_TRANSPORT_RECONNECT_RESULT_REQUESTED;
            app->reconnect_last_status_code = 0U;
        } else if (!app->reconnect_inflight) {
            app->reconnect_last_result = HID_TRANSPORT_RECONNECT_RESULT_NO_CANDIDATE;
            app->reconnect_last_status_code = 0U;
        }
    }

    if (input->usb_tx_blocked) {
        (void)memset(&output->usb_tx, 0, sizeof(output->usb_tx));
    } else {
        output->usb_tx.valid = usb_bridge_take_usb_tx(&app->usb_bridge, &output->usb_tx);
    }

    if (input->bt_tx_blocked) {
        (void)memset(&output->bt_tx, 0, sizeof(output->bt_tx));
    } else {
        output->bt_tx.valid = usb_bridge_take_bt_tx(&app->usb_bridge, &output->bt_tx);
    }
    (void)usb_bridge_telemetry_get(&app->usb_bridge, &bridge_telemetry);
    output->usb_tx_queue_depth = bridge_telemetry.usb_tx_depth;
    output->bt_tx_queue_depth = bridge_telemetry.bt_tx_depth;
    output->usb_tx_queue_high_watermark = bridge_telemetry.usb_tx_high_watermark;
    output->bt_tx_queue_high_watermark = bridge_telemetry.bt_tx_high_watermark;
    output->usb_tx_dropped = bridge_telemetry.usb_tx_dropped;
    output->bt_tx_dropped = bridge_telemetry.bt_tx_dropped;
    output->diag.bt_state = (uint8_t)bt_state;
    output->diag.active_device_count = output->active_device_count;
    output->diag.usb_interface_count = output->usb_interface_count;
    output->diag.usb_tx_depth = output->usb_tx_queue_depth;
    output->diag.bt_tx_depth = output->bt_tx_queue_depth;
    output->diag.usb_tx_high_watermark = output->usb_tx_queue_high_watermark;
    output->diag.bt_tx_high_watermark = output->bt_tx_queue_high_watermark;
    output->diag.usb_tx_dropped = output->usb_tx_dropped;
    output->diag.bt_tx_dropped = output->bt_tx_dropped;
    output->diag.stack_event_depth = 0U;
    output->diag.stack_event_high_watermark = 0U;
    output->diag.stack_event_dropped = 0U;
    output->diag.reconnect_attempt_count = app->reconnect_attempt_count;
    output->diag.reconnect_success_count = app->reconnect_success_count;
    output->diag.reconnect_failure_count = app->reconnect_failure_count;
    output->diag.reconnect_last_result = app->reconnect_last_result;
    output->diag.reconnect_last_status_code = app->reconnect_last_status_code;
    output->diag.stack_connect_pending = 0U;
    output->diag.stack_reconnect_pending = 0U;
    output->diag.stack_connect_mode = 0U;
    output->diag.stack_reconnect_attempt_index = 0U;
    output->diag.stack_reconnect_attempt_count = 0U;
    output->diag.stack_last_connect_status = 0U;
    output->forget_request = forget_request;

    if (app->factory_reset_armed && !input->button_pressed) {
        output->factory_reset_requested = true;
        app->factory_reset_armed = false;
    }
}
