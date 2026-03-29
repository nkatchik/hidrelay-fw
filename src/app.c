#include "app.h"

#include <stddef.h>
#include <string.h>

enum {
    APP_DEFAULT_SLEEP_MS = 10U,
    APP_REMOVE_LAST_MAX_AGE_MS = 60U * 60U * 1000U,
    APP_RECONNECT_TIMEOUT_MS = 8000U,
    APP_RECONNECT_BASE_BACKOFF_MS = 5000U,
    APP_RECONNECT_MAX_BACKOFF_SHIFT = 6U,
    APP_RECONNECT_STACK_REJECT_RETRY_MS = 1000U,
    APP_RECONNECT_STACK_NOT_READY_RETRY_MS = 3000U,
    APP_RECONNECT_FAIL_DISABLE_THRESHOLD = 8U,
    APP_REMOVE_LAST_BLINK_COUNT = 1U,
    APP_FACTORY_RESET_BLINK_COUNT = 3U,
    APP_FACTORY_RESET_SEQUENCE_MS = 2600U
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
    bool disable_reconnect = false;

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

    if ((reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_TIMEOUT)
        || (reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_CONNECT_FAILED)) {
        if (fail_count < UINT8_MAX) {
            fail_count = (uint8_t)(fail_count + 1U);
        }

        if (fail_count >= APP_RECONNECT_FAIL_DISABLE_THRESHOLD) {
            disable_reconnect = true;
        }

        retry_after_ms = now_ms + app_reconnect_backoff_ms(fail_count);
    } else if (reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_AUTH_FAILED) {
        if (fail_count < UINT8_MAX) {
            fail_count = (uint8_t)(fail_count + 1U);
        }

        disable_reconnect = true;
        retry_after_ms = now_ms + app_reconnect_backoff_ms(APP_RECONNECT_MAX_BACKOFF_SHIFT + 1U);
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
    if (disable_reconnect) {
        (void)pair_db_set_reconnect_allowed(&app->pair_db, &app->reconnect_device_id, false);
    }

    app->reconnect_failure_count = app->reconnect_failure_count + 1U;
    app->reconnect_last_result = reconnect_result;
    app->reconnect_last_status_code = status_code;
    app_reconnect_clear_inflight(app);
}

static void app_handle_transport_event(
    app_t * app,
    const app_input_t * input
) {
    const hid_transport_event_t * event = NULL;

    if ((app == NULL) || (input == NULL)) {
        return;
    }

    event = &input->transport_event;

    switch (event->type) {
        case HID_TRANSPORT_EVENT_BT_HID_OPEN:
            (void)bt_manager_ingest_hid_open(
                &app->bt_manager,
                &event->device_id,
                event->hid_cid,
                event->vendor_id,
                event->product_id,
                event->report_descriptor_len,
                input->now_ms
            );
            break;
        case HID_TRANSPORT_EVENT_BT_HID_CLOSE:
            (void)bt_manager_ingest_hid_close(&app->bt_manager, event->hid_cid, input->now_ms);
            break;
        case HID_TRANSPORT_EVENT_BT_HID_DESCRIPTOR:
            (void)bt_manager_ingest_hid_descriptor(
                &app->bt_manager,
                event->hid_cid,
                event->report_descriptor_len,
                input->now_ms
            );
            break;
        case HID_TRANSPORT_EVENT_BT_HID_PROTOCOL:
            (void)bt_manager_ingest_hid_protocol(
                &app->bt_manager,
                event->hid_cid,
                event->protocol_mode,
                input->now_ms
            );
            break;
        case HID_TRANSPORT_EVENT_BT_HID_REPORT:
            (void)usb_bridge_ingest_bt_report(
                &app->usb_bridge,
                event->hid_cid,
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
    app->factory_reset_due_ms = 0U;
}

void app_tick(
    app_t * app,
    const app_input_t * input,
    app_output_t * output
) {
    button_command_t command = BUTTON_COMMAND_NONE;
    pair_db_t pair_db_before = {0};
    usb_bridge_telemetry_t bridge_telemetry = {0};
    bt_manager_state_t bt_state = BT_MANAGER_STATE_IDLE;
    pair_db_entry_t reconnect_candidate = {0};
    uint8_t reconnect_index = 0U;
    uint8_t interface_index = 0U;

    if ((app == NULL) || (input == NULL) || (output == NULL)) {
        return;
    }

    pair_db_before = app->pair_db;
    command = button_fsm_update(&app->button_fsm, input->button_pressed, input->now_ms);

    if (command == BUTTON_COMMAND_PAIR_ANY) {
        (void)bt_manager_start_pair_any(&app->bt_manager, input->now_ms);
    } else if (command == BUTTON_COMMAND_REMOVE_LAST) {
        if (bt_manager_remove_last_if_recent(
                &app->bt_manager,
                input->now_ms,
                APP_REMOVE_LAST_MAX_AGE_MS
            )) {
            led_ui_trigger_long_blink(&app->led_ui, APP_REMOVE_LAST_BLINK_COUNT, input->now_ms);
        }
    } else if (command == BUTTON_COMMAND_REMOVE_ALL) {
        (void)bt_manager_remove_all(&app->bt_manager);
        led_ui_trigger_long_blink(&app->led_ui, APP_FACTORY_RESET_BLINK_COUNT, input->now_ms);
        app->factory_reset_armed = true;
        app->factory_reset_due_ms = input->now_ms + APP_FACTORY_RESET_SEQUENCE_MS;
    }

    app_handle_transport_event(app, input);

    if (app->reconnect_inflight
        && (input->transport_event.type == HID_TRANSPORT_EVENT_BT_HID_OPEN)
        && app_device_id_equal(&input->transport_event.device_id, &app->reconnect_device_id)) {
        app_reconnect_mark_success(app, &input->transport_event.device_id, input->now_ms);
    }

    if (app->reconnect_inflight
        && (input->transport_event.type == HID_TRANSPORT_EVENT_RECONNECT_RESULT)
        && app_device_id_equal(&input->transport_event.device_id, &app->reconnect_device_id)) {
        if (input->transport_event.reconnect_result == HID_TRANSPORT_RECONNECT_RESULT_SUCCESS) {
            app_reconnect_mark_success(app, &input->transport_event.device_id, input->now_ms);
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
    usb_bridge_sync_from_bt_manager(&app->usb_bridge, &app->bt_manager);
    usb_bridge_tick(&app->usb_bridge, input->now_ms);

    bt_state = bt_manager_state(&app->bt_manager);
    led_ui_set_state(&app->led_ui, app_led_state_from_bt_state(bt_state), input->now_ms);

    output->led_on = led_ui_tick(&app->led_ui, input->now_ms);
    output->pairing_active = bt_state == BT_MANAGER_STATE_PAIRING;
    output->sleep_ms = APP_DEFAULT_SLEEP_MS;
    output->usb_interface_count = usb_bridge_interface_count(&app->usb_bridge);
    output->usb_descriptor_generation = usb_bridge_descriptor_generation(&app->usb_bridge);
    output->active_device_count = bt_manager_active_count(&app->bt_manager);
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
    }

    output->reconnect_request.valid = false;
    output->factory_reset_requested = false;
    (void)memset(
        &output->reconnect_request.device_id,
        0,
        sizeof(output->reconnect_request.device_id)
    );

    if (app->reconnect_inflight
        && !pair_db_find(&app->pair_db, &app->reconnect_device_id, &reconnect_index)) {
        app_reconnect_clear_inflight(app);
    }

    if ((bt_state == BT_MANAGER_STATE_IDLE) && (output->active_device_count == 0U)) {
        if (app->reconnect_inflight
            && ((input->now_ms - app->reconnect_started_ms) >= APP_RECONNECT_TIMEOUT_MS)) {
            app_reconnect_mark_failure(
                app,
                HID_TRANSPORT_RECONNECT_RESULT_TIMEOUT,
                0U,
                input->now_ms
            );
        }

        if (!app->reconnect_inflight
            && pair_db_get_reconnect_candidate(
                &app->pair_db,
                input->now_ms,
                &reconnect_candidate
            )) {
            output->reconnect_request.valid = true;
            output->reconnect_request.device_id = reconnect_candidate.device_id;
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

    output->usb_tx.valid = usb_bridge_take_usb_tx(&app->usb_bridge, &output->usb_tx);
    output->bt_tx.valid = usb_bridge_take_bt_tx(&app->usb_bridge, &output->bt_tx);
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
    output->diag.reconnect_attempt_count = app->reconnect_attempt_count;
    output->diag.reconnect_success_count = app->reconnect_success_count;
    output->diag.reconnect_failure_count = app->reconnect_failure_count;
    output->diag.reconnect_last_result = app->reconnect_last_result;
    output->diag.reconnect_last_status_code = app->reconnect_last_status_code;

    if (app->factory_reset_armed && (input->now_ms >= app->factory_reset_due_ms)) {
        output->factory_reset_requested = true;
        app->factory_reset_armed = false;
        app->factory_reset_due_ms = 0U;
    }

    output->pair_db_dirty = memcmp(&pair_db_before, &app->pair_db, sizeof(pair_db_before)) != 0;
}
