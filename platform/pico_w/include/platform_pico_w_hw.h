#ifndef HIDRELAY_PLATFORM_PICO_W_HW_H
#define HIDRELAY_PLATFORM_PICO_W_HW_H

#include <stdbool.h>
#include <stdint.h>

bool pico_w_hw_init_radio(void);
bool pico_w_hw_bootsel_pressed(void);
uint32_t pico_w_hw_uptime_ms(void);
void pico_w_hw_set_led(bool led_on);
void pico_w_hw_sleep_us(uint32_t sleep_duration_us);
/* Disarm the hang-watchdog report before a deliberate reset (see
 * platform_take_hang_report); implemented next to the watchdog glue. */
void pico_w_hw_disarm_hang_report(void);

#endif
