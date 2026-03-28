#include "app.h"
#include "platform_api.h"

int main(void) {
    app_t app = {0};
    app_output_t app_output = {
        .led_on = false,
        .sleep_ms = 10U,
    };

    if (!platform_init()) {
        return 1;
    }

    app_init(&app);

    for (;;) {
        platform_input_t platform_input = {0};
        platform_output_t platform_output = {0};
        app_input_t app_input = {0};

        platform_poll(&platform_input);

        app_input.button_pressed = platform_input.button_pressed;
        app_input.now_ms = platform_input.uptime_ms;

        app_tick(&app, &app_input, &app_output);

        platform_output.led_on = app_output.led_on;
        platform_output.sleep_ms = app_output.sleep_ms;

        platform_apply(&platform_output);
    }
}
