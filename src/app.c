#include "app.h"

#include <stddef.h>
#include <string.h>

enum {
    APP_DEFAULT_SLEEP_MS = 10U,
    APP_REMOVE_LAST_MAX_AGE_MS = 60U * 60U * 1000U,
    APP_REMOVE_LAST_BLINK_COUNT = 1U,
    APP_FACTORY_RESET_BLINK_COUNT = 3U
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

static void app_seed_pair_db(pair_db_t *dst, const pair_db_t *src) {
    if ((dst == NULL) || (src == NULL)) {
        return;
    }

    *dst = *src;

    if (dst->count > PAIR_DB_MAX_DEVICE) {
        dst->count = PAIR_DB_MAX_DEVICE;
    }
}

static void app_handle_transport_event(app_t *app, const app_input_t *input) {
    const hid_transport_event_t *event = NULL;

    if ((app == NULL) || (input == NULL)) {
        return;
    }

    event = &input->transport_event;

    switch (event->type) {
    case HID_TRANSPORT_EVENT_BT_HID_OPEN:
        (void)bt_manager_ingest_hid_open(&app->bt_manager,
                                         &event->device_id,
                                         event->hid_cid,
                                         event->vendor_id,
                                         event->product_id,
                                         event->report_descriptor_len,
                                         input->now_ms);
        break;
    case HID_TRANSPORT_EVENT_BT_HID_CLOSE:
        (void)bt_manager_ingest_hid_close(&app->bt_manager, event->hid_cid, input->now_ms);
        break;
    case HID_TRANSPORT_EVENT_BT_HID_DESCRIPTOR:
        (void)bt_manager_ingest_hid_descriptor(&app->bt_manager,
                                               event->hid_cid,
                                               event->report_descriptor_len,
                                               input->now_ms);
        break;
    case HID_TRANSPORT_EVENT_BT_HID_PROTOCOL:
        (void)bt_manager_ingest_hid_protocol(&app->bt_manager, event->hid_cid, event->protocol_mode, input->now_ms);
        break;
    case HID_TRANSPORT_EVENT_BT_HID_REPORT:
        (void)usb_bridge_ingest_bt_report(&app->usb_bridge, event->hid_cid, event->report, event->report_len);
        break;
    case HID_TRANSPORT_EVENT_USB_HID_REPORT:
        (void)usb_bridge_ingest_usb_report(&app->usb_bridge,
                                           event->interface_number,
                                           event->report,
                                           event->report_len);
        break;
    case HID_TRANSPORT_EVENT_NONE:
    default:
        break;
    }
}

void app_init(app_t *app, const pair_db_t *initial_pair_db) {
    if (app == NULL) {
        return;
    }

    button_fsm_init(&app->button_fsm);
    led_ui_init(&app->led_ui);
    pair_db_init(&app->pair_db);
    app_seed_pair_db(&app->pair_db, initial_pair_db);
    bt_manager_init(&app->bt_manager, &app->pair_db);
    usb_bridge_init(&app->usb_bridge);
}

void app_tick(app_t *app, const app_input_t *input, app_output_t *output) {
    button_command_t command = BUTTON_COMMAND_NONE;
    pair_db_t pair_db_before = {0};
    usb_bridge_telemetry_t bridge_telemetry = {0};

    if ((app == NULL) || (input == NULL) || (output == NULL)) {
        return;
    }

    pair_db_before = app->pair_db;
    command = button_fsm_update(&app->button_fsm, input->button_pressed, input->now_ms);

    if (command == BUTTON_COMMAND_PAIR_ANY) {
        (void)bt_manager_start_pair_any(&app->bt_manager, input->now_ms);
    } else if (command == BUTTON_COMMAND_REMOVE_LAST) {
        if (bt_manager_remove_last_if_recent(&app->bt_manager, input->now_ms, APP_REMOVE_LAST_MAX_AGE_MS)) {
            led_ui_trigger_long_blink(&app->led_ui, APP_REMOVE_LAST_BLINK_COUNT, input->now_ms);
        }
    } else if (command == BUTTON_COMMAND_REMOVE_ALL) {
        (void)bt_manager_remove_all(&app->bt_manager);
        led_ui_trigger_long_blink(&app->led_ui, APP_FACTORY_RESET_BLINK_COUNT, input->now_ms);
    }

    app_handle_transport_event(app, input);
    bt_manager_tick(&app->bt_manager, input->now_ms);
    usb_bridge_sync_from_bt_manager(&app->usb_bridge, &app->bt_manager);
    usb_bridge_tick(&app->usb_bridge, input->now_ms);

    led_ui_set_state(&app->led_ui, app_led_state_from_bt_state(bt_manager_state(&app->bt_manager)), input->now_ms);

    output->led_on = led_ui_tick(&app->led_ui, input->now_ms);
    output->pairing_active = bt_manager_state(&app->bt_manager) == BT_MANAGER_STATE_PAIRING;
    output->sleep_ms = APP_DEFAULT_SLEEP_MS;
    output->usb_interface_count = usb_bridge_interface_count(&app->usb_bridge);
    output->usb_descriptor_generation = usb_bridge_descriptor_generation(&app->usb_bridge);
    output->usb_tx.valid = usb_bridge_take_usb_tx(&app->usb_bridge, &output->usb_tx);
    output->bt_tx.valid = usb_bridge_take_bt_tx(&app->usb_bridge, &output->bt_tx);
    (void)usb_bridge_telemetry_get(&app->usb_bridge, &bridge_telemetry);
    output->usb_tx_queue_depth = bridge_telemetry.usb_tx_depth;
    output->bt_tx_queue_depth = bridge_telemetry.bt_tx_depth;
    output->usb_tx_queue_high_watermark = bridge_telemetry.usb_tx_high_watermark;
    output->bt_tx_queue_high_watermark = bridge_telemetry.bt_tx_high_watermark;
    output->usb_tx_dropped = bridge_telemetry.usb_tx_dropped;
    output->bt_tx_dropped = bridge_telemetry.bt_tx_dropped;
    output->pair_db_dirty = memcmp(&pair_db_before, &app->pair_db, sizeof(pair_db_before)) != 0;
}
