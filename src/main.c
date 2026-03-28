#include "app.h"
#include "platform_api.h"

#include <stddef.h>

int main(void) {
    app_t app = {0};
    pair_db_t initial_pair_db = {0};
    const bool has_initial_pair_db = platform_pair_db_load(&initial_pair_db);
    app_output_t app_output = {
        .led_on = false,
        .sleep_ms = 10U,
        .usb_interface_count = 0U,
        .usb_descriptor_generation = 0U,
        .usb_tx = { .valid = false, .interface_number = 0U, .report_len = 0U, .report = {0} },
        .bt_tx = { .valid = false, .hid_cid = 0U, .report_len = 0U, .report = {0} },
        .pair_db_dirty = false,
    };

    if (!platform_init()) {
        return 1;
    }

    app_init(&app, has_initial_pair_db ? &initial_pair_db : NULL);

    for (;;) {
        platform_input_t platform_input = {0};
        platform_output_t platform_output = {0};
        app_input_t app_input = {0};

        platform_poll(&platform_input);

        app_input.button_pressed = platform_input.button_pressed;
        app_input.now_ms = platform_input.uptime_ms;
        app_input.transport_event = platform_input.transport_event;

        app_tick(&app, &app_input, &app_output);

        platform_output.led_on = app_output.led_on;
        platform_output.sleep_ms = app_output.sleep_ms;
        platform_output.usb_interface_count = app_output.usb_interface_count;
        platform_output.usb_descriptor_generation = app_output.usb_descriptor_generation;
        platform_output.usb_tx = app_output.usb_tx;
        platform_output.bt_tx = app_output.bt_tx;

        platform_apply(&platform_output);

        if (app_output.pair_db_dirty) {
            (void)platform_pair_db_save(&app.pair_db);
        }
    }
}
