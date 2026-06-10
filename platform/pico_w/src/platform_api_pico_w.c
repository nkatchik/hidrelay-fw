#include <stddef.h>

#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "platform_api.h"
#include "platform_pico_w_hw.h"
#include "platform_pico_w_state.h"

static pico_w_state_t g_state = {
    .initialized = false,
};

static bool platform_ready(void) {
    return pico_w_state_is_initialized(&g_state);
}

bool platform_init(void) {
#if defined(APP_HAS_TELEMETRY)
    stdio_init_all();
#endif
    pico_w_state_reset(&g_state);

    if (!pico_w_hw_init_radio()) {
        return false;
    }

    pico_w_state_mark_initialized(&g_state);
    return true;
}

bool platform_button_pressed(void) {
    if (!platform_ready()) {
        return false;
    }

    return pico_w_hw_bootsel_pressed();
}

uint32_t platform_uptime_ms(void) {
    return pico_w_hw_uptime_ms();
}

void platform_set_led(bool led_on) {
    if (!platform_ready()) {
        return;
    }

    pico_w_hw_set_led(led_on);
}

void platform_sleep_us(uint32_t sleep_us) {
    if (!platform_ready()) {
        return;
    }

    pico_w_hw_sleep_us(sleep_us);
}

void platform_reboot(void) {
    watchdog_reboot(0U, 0U, 0U);

    for (;;) {
        tight_loop_contents();
    }
}
