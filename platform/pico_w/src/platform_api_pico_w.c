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
 * Hang-watchdog breadcrumbs live in watchdog scratch registers 2 and 3,
 * which survive a watchdog reset but not power-on. Scratch 4-7 belong to
 * the SDK/bootrom: watchdog_enable() itself stamps scratch[4] with its
 * non-reboot magic (the first version of this code stored its own armed
 * marker there and watchdog_enable silently clobbered it -- hang reports
 * never fired). Armed-ness therefore rides the SDK's own mechanism:
 * watchdog_enable_caused_reboot() is true exactly when the reset came from
 * an armed watchdog timing out, and deliberate reboots
 * (watchdog_reboot/reset paths) overwrite the SDK magic on their own.
 */
enum {
    PICO_W_HANG_SCRATCH_BT = 2U,
    PICO_W_HANG_SCRATCH_MAIN = 3U,
    PICO_W_SDK_SCRATCH_REBOOT_MAGIC = 4U,
    PICO_W_WATCHDOG_TIMEOUT_MS = 5000U
};

static bool platform_ready(void) {
    return pico_w_state_is_initialized(&g_state);
}

void platform_watchdog_enable(void) {
    watchdog_hw->scratch[PICO_W_HANG_SCRATCH_BT] = 0U;
    watchdog_hw->scratch[PICO_W_HANG_SCRATCH_MAIN] = 0U;
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
    if (!watchdog_enable_caused_reboot()) {
        return false;
    }

    if (out_bt_marker != NULL) {
        *out_bt_marker = (uint8_t)watchdog_hw->scratch[PICO_W_HANG_SCRATCH_BT];
    }
    if (out_main_marker != NULL) {
        *out_main_marker = (uint8_t)watchdog_hw->scratch[PICO_W_HANG_SCRATCH_MAIN];
    }
    watchdog_hw->scratch[PICO_W_SDK_SCRATCH_REBOOT_MAGIC] = 0U;
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
    /* Clearing the SDK's reboot magic makes watchdog_enable_caused_reboot()
     * false on the next boot, so deliberate resets never blink a report. */
    watchdog_hw->scratch[PICO_W_SDK_SCRATCH_REBOOT_MAGIC] = 0U;
}

void platform_reboot(void) {
    /* Deliberate reboot: disarm the hang report before the watchdog reset. */
    pico_w_hw_disarm_hang_report();
    watchdog_reboot(0U, 0U, 0U);

    for (;;) {
        tight_loop_contents();
    }
}
