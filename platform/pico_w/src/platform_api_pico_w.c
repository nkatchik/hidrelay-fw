#include <stddef.h>
#include <string.h>

#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico.h"
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
    PICO_W_HANG_SCRATCH_PANIC = 0U,
    PICO_W_HANG_SCRATCH_PANIC_FMT = 1U,
    PICO_W_HANG_SCRATCH_BT = 2U,
    PICO_W_HANG_SCRATCH_MAIN = 3U,
    PICO_W_SDK_SCRATCH_REBOOT_MAGIC = 4U,
    /* RP2040 hardware ceiling: 24-bit microsecond load counter that
     * decrements twice per tick (errata RP2040-E1) caps the watchdog at
     * ~8.39 s. 8 s is the longest timeout this chip can enforce. */
    PICO_W_WATCHDOG_TIMEOUT_MS = 8000U
};

/* XIP flash window: panic format strings are literals living here, so the
 * recorded pointer is still readable after the watchdog reset. */
#define PICO_W_FLASH_BASE XIP_BASE
#define PICO_W_FLASH_LIMIT (XIP_BASE + PICO_FLASH_SIZE_BYTES)

/* "PAN" tag + class in the low byte (see pico_w_panic). */
#define PICO_W_PANIC_CODE_BASE 0x50414E00U
#define PICO_W_PANIC_CODE_MASK 0xFFFFFF00U

/*
 * All SDK panic() calls land here (PICO_PANIC_FUNCTION). Record which panic
 * fired in a scratch register that survives the watchdog reset, then spin
 * until the hang watchdog reboots the firmware; the boot-time hang report
 * blinks the class out. Class 1 = the cyw43 BT shared-bus RX overflow
 * panic, 2 = its register-corruption panic, 3 = any other panic.
 */
void __attribute__((noreturn)) pico_w_panic(
    const char * fmt,
    ...
) {
    uint32_t panic_class = 3U;

    if (fmt != NULL) {
        if (strstr(fmt, "buffer overflow") != NULL) {
            panic_class = 1U;
        } else if (strstr(fmt, "register corruption") != NULL) {
            panic_class = 2U;
        }
    }
    watchdog_hw->scratch[PICO_W_HANG_SCRATCH_PANIC] = PICO_W_PANIC_CODE_BASE | panic_class;
    watchdog_hw->scratch[PICO_W_HANG_SCRATCH_PANIC_FMT] = (uint32_t)fmt;

    (void)save_and_disable_interrupts();
    for (;;) {
        tight_loop_contents();
    }
}

static bool platform_ready(void) {
    return pico_w_state_is_initialized(&g_state);
}

void platform_watchdog_enable(void) {
    watchdog_hw->scratch[PICO_W_HANG_SCRATCH_PANIC] = 0U;
    watchdog_hw->scratch[PICO_W_HANG_SCRATCH_PANIC_FMT] = 0U;
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
    uint8_t * out_main_marker,
    uint8_t * out_panic_class,
    const char ** out_panic_text
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
    if (out_panic_class != NULL) {
        const uint32_t panic_code = watchdog_hw->scratch[PICO_W_HANG_SCRATCH_PANIC];

        *out_panic_class = ((panic_code & PICO_W_PANIC_CODE_MASK) == PICO_W_PANIC_CODE_BASE)
            ? (uint8_t)(panic_code & 0xFFU)
            : 0U;
    }
    if (out_panic_text != NULL) {
        const uint32_t fmt_addr = watchdog_hw->scratch[PICO_W_HANG_SCRATCH_PANIC_FMT];

        /*
         * The panic format string is a flash literal from the very image
         * still running, so the recorded pointer remains readable -- but
         * only trust it when it points into the XIP window.
         */
        *out_panic_text = ((fmt_addr >= PICO_W_FLASH_BASE) && (fmt_addr < PICO_W_FLASH_LIMIT))
            ? (const char *)fmt_addr
            : NULL;
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
