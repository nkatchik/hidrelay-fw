#include "platform_pico_w_hw.h"

#include "hardware/regs/io_qspi.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

enum {
    PLATFORM_PICO_W_BOOTSEL_QSPI_SS_INDEX = 1U
};

static bool g_pico_w_radio_initialized = false;

bool pico_w_hw_init_radio(void) {
    g_pico_w_radio_initialized = false;

    if (cyw43_arch_init() != 0) {
        return false;
    }

    g_pico_w_radio_initialized = true;
    return true;
}

static bool __no_inline_not_in_flash_func(pico_w_hw_bootsel_pressed_impl)(void) {
    const uint32_t irq_state = save_and_disable_interrupts();
    bool pressed = false;

    hw_write_masked(
        &ioqspi_hw->io[PLATFORM_PICO_W_BOOTSEL_QSPI_SS_INDEX].ctrl,
        (uint32_t)(IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_VALUE_DISABLE
            << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB),
        IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS
    );

    for (volatile uint32_t delay = 0U; delay < 128U; delay++) {
    }

    pressed = (sio_hw->gpio_hi_in & (1u << PLATFORM_PICO_W_BOOTSEL_QSPI_SS_INDEX)) == 0U;

    hw_write_masked(
        &ioqspi_hw->io[PLATFORM_PICO_W_BOOTSEL_QSPI_SS_INDEX].ctrl,
        (uint32_t)(IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_VALUE_NORMAL
            << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB),
        IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS
    );

    restore_interrupts(irq_state);
    return pressed;
}

bool pico_w_hw_bootsel_pressed(void) {
    return pico_w_hw_bootsel_pressed_impl();
}

uint32_t pico_w_hw_uptime_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

void pico_w_hw_set_led(bool led_on) {
    if (!g_pico_w_radio_initialized) {
        return;
    }

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on ? 1 : 0);
}

void pico_w_hw_sleep_us(uint32_t sleep_duration_us) {
    sleep_us(sleep_duration_us);
}
