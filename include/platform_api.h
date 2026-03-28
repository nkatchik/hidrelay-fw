#ifndef HIDRELAY_PLATFORM_API_H
#define HIDRELAY_PLATFORM_API_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool button_pressed;
    uint32_t uptime_ms;
} platform_input_t;

typedef struct {
    bool led_on;
    uint32_t sleep_ms;
    uint8_t usb_interface_count;
    uint32_t usb_descriptor_generation;
} platform_output_t;

bool platform_init(void);
void platform_poll(platform_input_t *input);
void platform_apply(const platform_output_t *output);

#endif
