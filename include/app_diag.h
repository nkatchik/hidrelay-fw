#ifndef HIDRELAY_APP_DIAG_H
#define HIDRELAY_APP_DIAG_H

#include <stdbool.h>
#include <stdint.h>

#include "hid_transport.h"

#define APP_DIAG_FRAME_MAX_LEN 49U

void app_diag_init(void);
void app_diag_publish(const hid_transport_diag_snapshot_t * diag);
bool app_diag_take(hid_transport_diag_snapshot_t * out_diag);
uint16_t app_diag_encode_frame(
    const hid_transport_diag_snapshot_t * diag,
    uint8_t * frame,
    uint16_t frame_capacity
);

#endif
