#ifndef HIDRELAY_PLATFORM_PICO_W_STACK_H
#define HIDRELAY_PLATFORM_PICO_W_STACK_H

#include <stdbool.h>
#include <stdint.h>

bool pico_w_stack_init(void);
void pico_w_stack_poll(uint32_t now_ms);
void pico_w_stack_set_usb_plan(uint8_t interface_count, uint32_t descriptor_generation);
uint8_t pico_w_stack_usb_interface_count(void);

#endif
