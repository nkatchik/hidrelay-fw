#ifndef HIDRELAY_PLATFORM_PICO_W_STACK_H
#define HIDRELAY_PLATFORM_PICO_W_STACK_H

#include <stdbool.h>
#include <stdint.h>

bool pico_w_stack_init(void);
void pico_w_stack_poll(uint32_t now_ms);

#endif
