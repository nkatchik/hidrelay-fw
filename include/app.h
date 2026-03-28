#ifndef HIDRELAY_APP_H
#define HIDRELAY_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "bt_manager.h"
#include "button_fsm.h"
#include "led_ui.h"
#include "pair_db.h"
#include "usb_bridge.h"

typedef struct {
    bool button_pressed;
    uint32_t now_ms;
} app_input_t;

typedef struct {
    bool led_on;
    uint32_t sleep_ms;
} app_output_t;

typedef struct {
    button_fsm_t button_fsm;
    led_ui_t led_ui;
    pair_db_t pair_db;
    bt_manager_t bt_manager;
    usb_bridge_t usb_bridge;
} app_t;

void app_init(app_t *app);
void app_tick(app_t *app, const app_input_t *input, app_output_t *output);

#endif
