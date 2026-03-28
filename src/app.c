#include "app.h"

#include <stddef.h>

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

void app_init(app_t *app) {
    if (app == NULL) {
        return;
    }

    button_fsm_init(&app->button_fsm);
    led_ui_init(&app->led_ui);
    pair_db_init(&app->pair_db);
    bt_manager_init(&app->bt_manager, &app->pair_db);
    usb_bridge_init(&app->usb_bridge);
}

void app_tick(app_t *app, const app_input_t *input, app_output_t *output) {
    button_command_t command = BUTTON_COMMAND_NONE;

    if ((app == NULL) || (input == NULL) || (output == NULL)) {
        return;
    }

    command = button_fsm_update(&app->button_fsm, input->button_pressed, input->now_ms);

    if (command == BUTTON_COMMAND_PAIR_ANY) {
        (void)bt_manager_start_pair_any(&app->bt_manager, input->now_ms);
    } else if (command == BUTTON_COMMAND_REMOVE_LAST) {
        if (bt_manager_remove_last_if_recent(&app->bt_manager, input->now_ms, APP_REMOVE_LAST_MAX_AGE_MS)) {
            usb_bridge_sync_from_pair_db(&app->usb_bridge, &app->pair_db);
            led_ui_trigger_long_blink(&app->led_ui, APP_REMOVE_LAST_BLINK_COUNT, input->now_ms);
        }
    } else if (command == BUTTON_COMMAND_REMOVE_ALL) {
        (void)bt_manager_remove_all(&app->bt_manager);
        usb_bridge_sync_from_pair_db(&app->usb_bridge, &app->pair_db);
        led_ui_trigger_long_blink(&app->led_ui, APP_FACTORY_RESET_BLINK_COUNT, input->now_ms);
    }

    bt_manager_tick(&app->bt_manager, input->now_ms);
    usb_bridge_sync_from_pair_db(&app->usb_bridge, &app->pair_db);
    usb_bridge_tick(&app->usb_bridge, input->now_ms);

    led_ui_set_state(&app->led_ui, app_led_state_from_bt_state(bt_manager_state(&app->bt_manager)), input->now_ms);

    output->led_on = led_ui_tick(&app->led_ui, input->now_ms);
    output->sleep_ms = APP_DEFAULT_SLEEP_MS;
}
