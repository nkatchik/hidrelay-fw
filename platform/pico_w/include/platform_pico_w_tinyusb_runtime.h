#ifndef HIDRELAY_PLATFORM_PICO_W_TINYUSB_RUNTIME_H
#define HIDRELAY_PLATFORM_PICO_W_TINYUSB_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

bool pico_w_tinyusb_runtime_init(void);
bool pico_w_tinyusb_runtime_is_initialized(void);
void pico_w_tinyusb_runtime_poll(void);
void pico_w_tinyusb_runtime_mark_descriptor_activity(void);
uint32_t pico_w_tinyusb_runtime_descriptor_activity_count(void);
bool pico_w_tinyusb_runtime_send_in_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
);
void pico_w_tinyusb_runtime_request_reenumeration(void);
bool pico_w_tinyusb_runtime_diag_write(
    const uint8_t * data,
    uint16_t data_len
);

#endif
