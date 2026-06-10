#include <stddef.h>

#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "platform_api.h"
#include "platform_pico_w_hw.h"
#include "platform_pico_w_state.h"

static pico_w_state_t g_state = {
    .initialized = false,
};

/*
 * Hang-watchdog breadcrumbs live in watchdog scratch registers, which
 * survive a watchdog reset but not power-on. scratch[4] holds a magic value
 * while the watchdog is armed; deliberate reboots clear it so only genuine
 * hangs produce a report.
 */
enum {
    PICO_W_HANG_SCRATCH_BT = 2U,
    PICO_W_HANG_SCRATCH_MAIN = 3U,
    PICO_W_HANG_SCRATCH_ARMED = 4U,
    PICO_W_HANG_ARMED_MAGIC = 0x48414E47U, /* "HANG" */
    PICO_W_WATCHDOG_TIMEOUT_MS = 5000U
};

static bool platform_ready(void) {
    return pico_w_state_is_initialized(&g_state);
}

void platform_watchdog_enable(void) {
    watchdog_hw->scratch[PICO_W_HANG_SCRATCH_BT] = 0U;
    watchdog_hw->scratch[PICO_W_HANG_SCRATCH_MAIN] = 0U;
    watchdog_hw->scratch[PICO_W_HANG_SCRATCH_ARMED] = PICO_W_HANG_ARMED_MAGIC;
    watchdog_enable(PICO_W_WATCHDOG_TIMEOUT_MS, true);
}

void platform_watchdog_feed(void) {
    watchdog_update();
}

void platform_hang_checkpoint_bt(uint8_t marker) {
    watchdog_hw->scratch[PICO_W_HANG_SCRATCH_BT] = marker;
}

void platform_hang_checkpoint_main(uint8_t marker) {
    watchdog_hw->scratch[PICO_W_HANG_SCRATCH_MAIN] = marker;
}

bool platform_take_hang_report(
    uint8_t * out_bt_marker,
    uint8_t * out_main_marker
) {
    if (!watchdog_caused_reboot()
        || (watchdog_hw->scratch[PICO_W_HANG_SCRATCH_ARMED] != PICO_W_HANG_ARMED_MAGIC)) {
        return false;
    }

    if (out_bt_marker != NULL) {
        *out_bt_marker = (uint8_t)watchdog_hw->scratch[PICO_W_HANG_SCRATCH_BT];
    }
    if (out_main_marker != NULL) {
        *out_main_marker = (uint8_t)watchdog_hw->scratch[PICO_W_HANG_SCRATCH_MAIN];
    }
    watchdog_hw->scratch[PICO_W_HANG_SCRATCH_ARMED] = 0U;
    return true;
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

void pico_w_hw_disarm_hang_report(void) {
    watchdog_hw->scratch[PICO_W_HANG_SCRATCH_ARMED] = 0U;
}

void platform_reboot(void) {
    /* Deliberate reboot: disarm the hang report before the watchdog reset. */
    pico_w_hw_disarm_hang_report();
    watchdog_reboot(0U, 0U, 0U);

    for (;;) {
        tight_loop_contents();
    }
}
