#ifndef HIDRELAY_PLATFORM_PICO_W_TINYUSB_RUNTIME_H
#define HIDRELAY_PLATFORM_PICO_W_TINYUSB_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "app.h"

bool pico_w_tinyusb_runtime_init(void);
void pico_w_tinyusb_runtime_poll(void);
bool pico_w_tinyusb_runtime_send_in_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
);
bool pico_w_tinyusb_runtime_diag_write(
    const uint8_t * data,
    uint16_t data_len
);
bool pico_w_tinyusb_runtime_take_operator_command(app_operator_command_t * out_command);

#endif
