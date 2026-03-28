#include "platform_api.h"

#include <stddef.h>

#include "pico/stdlib.h"

#include "platform_pico_w_hw.h"
#include "platform_pico_w_stack.h"
#include "platform_pico_w_state.h"

static pico_w_state_t g_state = {
    .initialized = false,
};

bool platform_init(void) {
    stdio_init_all();
    pico_w_state_reset(&g_state);

    if (!pico_w_hw_init_radio()) {
        return false;
    }

    if (!pico_w_stack_init()) {
        return false;
    }

    pico_w_state_mark_initialized(&g_state);
    return true;
}

void platform_poll(platform_input_t *input) {
    if ((input == NULL) || !pico_w_state_is_initialized(&g_state)) {
        return;
    }

    input->button_pressed = pico_w_hw_bootsel_pressed();
    input->uptime_ms = pico_w_hw_uptime_ms();
    pico_w_stack_poll(input->uptime_ms);
}

void platform_apply(const platform_output_t *output) {
    if ((output == NULL) || !pico_w_state_is_initialized(&g_state)) {
        return;
    }

    pico_w_hw_set_led(output->led_on);
    pico_w_hw_sleep_ms(output->sleep_ms);
}
