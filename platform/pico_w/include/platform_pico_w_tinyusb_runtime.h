#ifndef HIDRELAY_PLATFORM_PICO_W_TINYUSB_RUNTIME_H
#define HIDRELAY_PLATFORM_PICO_W_TINYUSB_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

bool pico_w_tinyusb_runtime_init(void);
void pico_w_tinyusb_runtime_poll(void);
bool pico_w_tinyusb_runtime_send_in_report(uint8_t interface_number, const uint8_t *report, uint16_t report_len);

#endif
