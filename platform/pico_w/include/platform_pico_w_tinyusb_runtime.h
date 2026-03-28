#ifndef HIDRELAY_PLATFORM_PICO_W_TINYUSB_RUNTIME_H
#define HIDRELAY_PLATFORM_PICO_W_TINYUSB_RUNTIME_H

#include <stdbool.h>

bool pico_w_tinyusb_runtime_init(void);
void pico_w_tinyusb_runtime_poll(void);

#endif
